#include <mbgl/map/map_context.hpp>
#include <mbgl/map/map_data.hpp>
#include <mbgl/map/view.hpp>
#include <mbgl/map/environment.hpp>
#include <mbgl/map/source.hpp>
#include <mbgl/map/sprite.hpp>

#include <mbgl/platform/log.hpp>

#include <mbgl/renderer/painter.hpp>

#include <mbgl/text/glyph_store.hpp>

#include <mbgl/geometry/glyph_atlas.hpp>
#include <mbgl/geometry/sprite_atlas.hpp>
#include <mbgl/geometry/line_atlas.hpp>

#include <mbgl/storage/resource.hpp>
#include <mbgl/storage/response.hpp>

#include <mbgl/style/style.hpp>

#include <mbgl/util/std.hpp>
#include <mbgl/util/uv_detail.hpp>
#include <mbgl/util/worker.hpp>
#include <mbgl/util/texture_pool.hpp>
#include <mbgl/util/mapbox.hpp>

namespace mbgl {

MapContext::MapContext(Environment& env_, View& view_, MapData& data_)
    : env(env_),
      view(view_),
      data(data_),
      updated(static_cast<UpdateType>(Update::Nothing)),
      glyphStore(util::make_unique<GlyphStore>(env)),
      glyphAtlas(util::make_unique<GlyphAtlas>(1024, 1024)),
      spriteAtlas(util::make_unique<SpriteAtlas>(512, 512)),
      lineAtlas(util::make_unique<LineAtlas>(512, 512)),
      texturePool(util::make_unique<TexturePool>()),
      painter(util::make_unique<Painter>(*spriteAtlas, *glyphAtlas, *lineAtlas)) {
}

void MapContext::start() {
    // Setup async notifications
    assert(!asyncTerminate);
    asyncTerminate = util::make_unique<uv::async>(env.loop, [this]() {
        assert(Environment::currentlyOn(ThreadType::Map));

        // Remove all of these to make sure they are destructed in the correct thread.
        style.reset();

        // It's now safe to destroy/join the workers since there won't be any more callbacks that
        // could dispatch to the worker pool.
        workers.reset();

        terminating = true;

        // Closes all open handles on the loop. This means that the loop will automatically terminate.
        asyncRender.reset();
        asyncUpdate.reset();
        asyncInvoke.reset();
        asyncTerminate.reset();
    });

    assert(!asyncUpdate);
    asyncUpdate = util::make_unique<uv::async>(env.loop, [this] {
        update();
    });

    assert(!asyncInvoke);
    asyncInvoke = util::make_unique<uv::async>(env.loop, [this] {
        processTasks();
    });

    assert(!asyncRender);
    asyncRender = util::make_unique<uv::async>(env.loop, [this] {
        // Must be called in Map thread.
        assert(Environment::currentlyOn(ThreadType::Map));

        render();

        // Finally, notify all listeners that we have finished rendering this frame.
        rendered.notify();
    });
}


void MapContext::terminate() {
    assert(asyncTerminate);
    asyncTerminate->send();
}

void MapContext::triggerRender() {
    assert(asyncRender);
    asyncRender->send();
}

void MapContext::triggerUpdate(const Update u) {
    updated |= static_cast<UpdateType>(u);

    if (data.mode == MapMode::Static) {
        prepare();
    } else if (asyncUpdate) {
        asyncUpdate->send();
    }
}

// Runs the function in the map thread.
void MapContext::invokeTask(std::function<void()>&& fn) {
    {
        std::lock_guard<std::mutex> lock(mutexTask);
        tasks.emplace(::std::forward<std::function<void()>>(fn));
    }

    // TODO: Once we have aligned static and continuous rendering, this should always dispatch
    // to the async queue.
    if (asyncInvoke) {
        asyncInvoke->send();
    } else {
        processTasks();
    }
}

Worker& MapContext::getWorker() {
    assert(workers);
    return *workers;
}

util::ptr<Sprite> MapContext::getSprite() {
    assert(Environment::currentlyOn(ThreadType::Map));
    const float pixelRatio = data.getTransformState().getPixelRatio();
    const std::string &sprite_url = style->getSpriteURL();
    if (!sprite || !sprite->hasPixelRatio(pixelRatio)) {
        sprite = Sprite::Create(sprite_url, pixelRatio, env);
    }

    return sprite;
}

void MapContext::reloadStyle() {
    assert(Environment::currentlyOn(ThreadType::Map));

    style = std::make_shared<Style>();

    const auto styleInfo = data.getStyleInfo();

    if (!styleInfo.url.empty()) {
        const auto base = styleInfo.base;
        // We have a style URL
        env.request({ Resource::Kind::JSON, styleInfo.url }, [this, base](const Response &res) {
            if (res.status == Response::Successful) {
                loadStyleJSON(res.data, base);
            } else {
                Log::Error(Event::Setup, "loading style failed: %s", res.message.c_str());
            }
        });
    } else {
        // We got JSON data directly.
        loadStyleJSON(styleInfo.json, styleInfo.base);
    }
}
void MapContext::loadStyleJSON(const std::string& json, const std::string& base) {
    assert(Environment::currentlyOn(ThreadType::Map));

    sprite.reset();
    style = std::make_shared<Style>();
    style->base = base;
    style->loadJSON((const uint8_t *)json.c_str());
    style->cascade(data.getClasses());
    style->setDefaultTransitionDuration(data.getDefaultTransitionDuration());

    const std::string glyphURL = util::mapbox::normalizeGlyphsURL(style->glyph_url, data.getAccessToken());
    glyphStore->setURL(glyphURL);

    for (const auto& source : style->sources) {
        source->load(data.getAccessToken(), env, [this]() {
            assert(Environment::currentlyOn(ThreadType::Map));
            triggerUpdate();
        });
    }

    triggerUpdate(Update::Zoom);
}

void MapContext::updateTiles() {
    assert(Environment::currentlyOn(ThreadType::Map));
    if (!style) return;
    for (const auto& source : style->sources) {
        source->update(data, getWorker(), style, *glyphAtlas, *glyphStore, *spriteAtlas,
                       getSprite(), *texturePool, [this]() {
            assert(Environment::currentlyOn(ThreadType::Map));
            triggerUpdate();
        });
    }
}
void MapContext::updateAnnotationTiles(const std::vector<TileID>& ids) {
    assert(Environment::currentlyOn(ThreadType::Main));
    if (!style) return;
    for (const auto &source : style->sources) {
        if (source->info.type == SourceType::Annotations) {
            source->invalidateTiles(ids);
        }
    }
    triggerUpdate();
}

void MapContext::update() {
    assert(Environment::currentlyOn(ThreadType::Map));

    if (data.getTransformState().hasSize()) {
        prepare();
    }
}


void MapContext::prepare() {
    assert(Environment::currentlyOn(ThreadType::Map));

    const auto now = Clock::now();
    data.setAnimationTime(now);

    auto u = updated.exchange(static_cast<UpdateType>(Update::Nothing)) |
             data.transform.updateTransitions(now);

    if (!style) {
        u |= static_cast<UpdateType>(Update::StyleInfo);
    }

    data.setTransformState(data.transform.currentState());

    if (u & static_cast<UpdateType>(Update::StyleInfo)) {
        reloadStyle();
    }

    if (u & static_cast<UpdateType>(Update::Debug)) {
        assert(painter);
        painter->setDebug(data.getDebug());
    }

    if (style) {
        if (u & static_cast<UpdateType>(Update::DefaultTransitionDuration)) {
            style->setDefaultTransitionDuration(data.getDefaultTransitionDuration());
        }

        if (u & static_cast<UpdateType>(Update::Classes)) {
            style->cascade(data.getClasses());
        }

        if (u & static_cast<UpdateType>(Update::StyleInfo) ||
            u & static_cast<UpdateType>(Update::Classes) ||
            u & static_cast<UpdateType>(Update::Zoom)) {
            style->recalculate(data.getTransformState().getNormalizedZoom(), now);
        }

        // Allow the sprite atlas to potentially pull new sprite images if needed.
        spriteAtlas->resize(data.getTransformState().getPixelRatio());
        spriteAtlas->setSprite(getSprite());

        updateTiles();
    }

    if (data.mode == MapMode::Continuous) {
        view.invalidate([this] { render(); });
    }
}

void MapContext::render() {
    assert(Environment::currentlyOn(ThreadType::Map));

    // Cleanup OpenGL objects that we abandoned since the last render call.
    env.performCleanup();

    assert(style);
    assert(painter);

    painter->render(*style, data.getTransformState(), data.getAnimationTime());

    // Schedule another rerender when we definitely need a next frame.
    if (data.transform.needsTransition() || style->hasTransitions()) {
        triggerUpdate();
    }
}


// Processes the functions that should be run in the map thread.
void MapContext::processTasks() {
    std::queue<std::function<void()>> queue;
    {
        std::lock_guard<std::mutex> lock(mutexTask);
        queue.swap(tasks);
    }

    while (!queue.empty()) {
        queue.front()();
        queue.pop();
    }
}

}
