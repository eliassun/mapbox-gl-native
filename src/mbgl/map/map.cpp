#include <mbgl/map/map.hpp>
#include <mbgl/map/update.hpp>
#include <mbgl/map/environment.hpp>
#include <mbgl/map/map_context.hpp>
#include <mbgl/map/view.hpp>
#include <mbgl/map/map_data.hpp>
#include <mbgl/platform/platform.hpp>
#include <mbgl/map/source.hpp>
#include <mbgl/renderer/painter.hpp>
#include <mbgl/map/annotation.hpp>
#include <mbgl/map/sprite.hpp>
#include <mbgl/util/math.hpp>
#include <mbgl/util/clip_id.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/uv_detail.hpp>
#include <mbgl/util/std.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/text/glyph_store.hpp>
#include <mbgl/geometry/glyph_atlas.hpp>
#include <mbgl/style/style_layer.hpp>
#include <mbgl/style/style_bucket.hpp>
#include <mbgl/util/texture_pool.hpp>
#include <mbgl/geometry/sprite_atlas.hpp>
#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/storage/file_source.hpp>
#include <mbgl/platform/log.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/uv.hpp>
#include <mbgl/util/mapbox.hpp>
#include <mbgl/util/exception.hpp>
#include <mbgl/util/worker.hpp>

#include <algorithm>
#include <iostream>

#define _USE_MATH_DEFINES
#include <cmath>

#include <uv.h>

// Check libuv library version.
const static bool uvVersionCheck = []() {
    const unsigned int version = uv_version();
    const unsigned int major = (version >> 16) & 0xFF;
    const unsigned int minor = (version >> 8) & 0xFF;
    const unsigned int patch = version & 0xFF;

#ifndef UV_VERSION_PATCH
    // 0.10 doesn't have UV_VERSION_PATCH defined, so we "fake" it by using the library patch level.
    const unsigned int UV_VERSION_PATCH = version & 0xFF;
#endif

    if (major != UV_VERSION_MAJOR || minor != UV_VERSION_MINOR || patch != UV_VERSION_PATCH) {
        throw std::runtime_error(mbgl::util::sprintf<96>(
            "libuv version mismatch: headers report %d.%d.%d, but library reports %d.%d.%d", UV_VERSION_MAJOR,
            UV_VERSION_MINOR, UV_VERSION_PATCH, major, minor, patch));
    }
    return true;
}();

namespace mbgl {

Map::Map(View& view_, FileSource& fileSource_)
    : env(util::make_unique<Environment>(fileSource_)),
      scope(util::make_unique<EnvironmentScope>(*env, ThreadType::Main, "Main")),
      view(view_),
      data(util::make_unique<MapData>(view_)),
      context(util::make_unique<MapContext>(*env, view, *data))
{
    view.initialize(this);
}

Map::~Map() {
    if (data->mode == MapMode::Continuous) {
        stop();
    }

    // Extend the scope to include both Main and Map thread types to ease cleanup.
    scope.reset();
    scope = util::make_unique<EnvironmentScope>(
        *env, static_cast<ThreadType>(static_cast<uint8_t>(ThreadType::Main) |
                                      static_cast<uint8_t>(ThreadType::Map)),
        "MapandMain");

    // Explicitly reset all pointers.
    context->style.reset();
    context.reset();

    uv_run(env->loop, UV_RUN_DEFAULT);

    env->performCleanup();
}

void Map::start(bool startPaused) {
    assert(Environment::currentlyOn(ThreadType::Main));
    assert(data->mode == MapMode::None);

    // When starting map rendering in another thread, we perform async/continuously
    // updated rendering. Only in these cases, we attach the async handlers.
    data->mode = MapMode::Continuous;

    // Reset the flag.
    isStopped = false;

    context->start();

    // Do we need to pause first?
    if (startPaused) {
        pause();
    }

    thread = std::thread([this]() {
#ifdef __APPLE__
        pthread_setname_np("Map");
#endif

        run();

        // Make sure that the stop() function knows when to stop invoking the callback function.
        isStopped = true;
        view.notify();
    });

    context->triggerUpdate();
}

void Map::stop(std::function<void ()> callback) {
    assert(Environment::currentlyOn(ThreadType::Main));
    assert(data->mode == MapMode::Continuous);

    context->terminate();

    resume();

    if (callback) {
        // Wait until the render thread stopped. We are using this construct instead of plainly
        // relying on the thread_join because the system might need to run things in the current
        // thread that is required for the render thread to terminate correctly. This is for example
        // the case with Cocoa's NSURLRequest. Otherwise, we will eventually deadlock because this
        // thread (== main thread) is blocked. The callback function should use an efficient waiting
        // function to avoid a busy waiting loop.
        while (!isStopped) {
            callback();
        }
    }

    // If a callback function was provided, this should return immediately because the thread has
    // already finished executing.
    thread.join();

    data->mode = MapMode::None;
}

void Map::pause(bool waitForPause) {
    assert(Environment::currentlyOn(ThreadType::Main));
    assert(data->mode == MapMode::Continuous);
    mutexRun.lock();
    pausing = true;
    mutexRun.unlock();

    uv_stop(env->loop);
    context->triggerUpdate(); // Needed to ensure uv_stop is seen and uv_run exits, otherwise we deadlock on wait_for_pause

    if (waitForPause) {
        std::unique_lock<std::mutex> lockPause (mutexPause);
        while (!isPaused) {
            condPause.wait(lockPause);
        }
    }
}

void Map::resume() {
    assert(Environment::currentlyOn(ThreadType::Main));
    assert(data->mode == MapMode::Continuous);

    mutexRun.lock();
    pausing = false;
    condRun.notify_all();
    mutexRun.unlock();
}

void Map::run() {
    ThreadType threadType = ThreadType::Map;
    std::string threadName("Map");

    if (data->mode == MapMode::None) {
        data->mode = MapMode::Static;

        // FIXME: Threads should have only one purpose. When running on Static mode,
        // we are currently not spawning a Map thread and running the code on the
        // Main thread, thus, the Main thread in this case is both Main and Map thread.
        threadType = static_cast<ThreadType>(static_cast<uint8_t>(threadType) | static_cast<uint8_t>(ThreadType::Main));
        threadName += "andMain";
    }

    EnvironmentScope mapScope(*env, threadType, threadName);

    if (data->mode == MapMode::Continuous) {
        checkForPause();
    }

    auto styleInfo = data->getStyleInfo();

    if (data->mode == MapMode::Static && !context->style && (styleInfo.url.empty() && styleInfo.json.empty())) {
        throw util::Exception("Style is not set");
    }

    view.activate();

    context->workers = util::make_unique<Worker>(env->loop, 4);

    setup();
    context->prepare();

    if (data->mode == MapMode::Continuous) {
        context->terminating = false;
        while (!context->terminating) {
            uv_run(env->loop, UV_RUN_DEFAULT);
            checkForPause();
        }
    } else {
        uv_run(env->loop, UV_RUN_DEFAULT);
    }

    // Run the event loop once more to make sure our async delete handlers are called.
    uv_run(env->loop, UV_RUN_ONCE);

    // If the map rendering wasn't started asynchronously, we perform one render
    // *after* all events have been processed.
    if (data->mode == MapMode::Static) {
        context->render();
        data->mode = MapMode::None;
    }

    view.deactivate();
}

void Map::renderSync() {
    // Must be called in UI thread.
    assert(Environment::currentlyOn(ThreadType::Main));

    context->triggerRender();

    context->rendered.wait();
}

void Map::checkForPause() {
    std::unique_lock<std::mutex> lockRun (mutexRun);
    while (pausing) {
        view.deactivate();

        mutexPause.lock();
        isPaused = true;
        condPause.notify_all();
        mutexPause.unlock();

        condRun.wait(lockRun);

        view.activate();
    }

    mutexPause.lock();
    isPaused = false;
    mutexPause.unlock();
}

void Map::update() {
    context->triggerUpdate();
}

#pragma mark - Setup

void Map::setup() {
    assert(Environment::currentlyOn(ThreadType::Map));
    assert(context->painter);
    context->painter->setup();
}

std::string Map::getStyleURL() const {
    return data->getStyleInfo().url;
}

void Map::setStyleURL(const std::string &url) {
    assert(Environment::currentlyOn(ThreadType::Main));

    const std::string styleURL = mbgl::util::mapbox::normalizeStyleURL(url, getAccessToken());

    const size_t pos = styleURL.rfind('/');
    std::string base = "";
    if (pos != std::string::npos) {
        base = styleURL.substr(0, pos + 1);
    }

    data->setStyleInfo({ styleURL, base, "" });
    context->triggerUpdate(Update::StyleInfo);
}

void Map::setStyleJSON(const std::string& json, const std::string& base) {
    assert(Environment::currentlyOn(ThreadType::Main));

    data->setStyleInfo({ "", base, json });
    context->triggerUpdate(Update::StyleInfo);
}

std::string Map::getStyleJSON() const {
    return data->getStyleInfo().json;
}

#pragma mark - Size

void Map::resize(uint16_t width, uint16_t height, float ratio) {
    resize(width, height, ratio, width * ratio, height * ratio);
}

void Map::resize(uint16_t width, uint16_t height, float ratio, uint16_t fbWidth, uint16_t fbHeight) {
    if (data->transform.resize(width, height, ratio, fbWidth, fbHeight)) {
        context->triggerUpdate();
    }
}

#pragma mark - Transitions

void Map::cancelTransitions() {
    data->transform.cancelTransitions();
    context->triggerUpdate();
}

void Map::setGestureInProgress(bool inProgress) {
    data->transform.setGestureInProgress(inProgress);
    context->triggerUpdate();
}

#pragma mark - Position

void Map::moveBy(double dx, double dy, Duration duration) {
    data->transform.moveBy(dx, dy, duration);
    context->triggerUpdate();
}

void Map::setLatLng(LatLng latLng, Duration duration) {
    data->transform.setLatLng(latLng, duration);
    context->triggerUpdate();
}

LatLng Map::getLatLng() const {
    return data->transform.getLatLng();
}

void Map::resetPosition() {
    data->transform.setAngle(0);
    data->transform.setLatLng(LatLng(0, 0));
    data->transform.setZoom(0);
    context->triggerUpdate(Update::Zoom);
}


#pragma mark - Scale

void Map::scaleBy(double ds, double cx, double cy, Duration duration) {
    data->transform.scaleBy(ds, cx, cy, duration);
    context->triggerUpdate(Update::Zoom);
}

void Map::setScale(double scale, double cx, double cy, Duration duration) {
    data->transform.setScale(scale, cx, cy, duration);
    context->triggerUpdate(Update::Zoom);
}

double Map::getScale() const {
    return data->transform.getScale();
}

void Map::setZoom(double zoom, Duration duration) {
    data->transform.setZoom(zoom, duration);
    context->triggerUpdate(Update::Zoom);
}

double Map::getZoom() const {
    return data->transform.getZoom();
}

void Map::setLatLngZoom(LatLng latLng, double zoom, Duration duration) {
    data->transform.setLatLngZoom(latLng, zoom, duration);
    context->triggerUpdate(Update::Zoom);
}

void Map::resetZoom() {
    setZoom(0);
}

double Map::getMinZoom() const {
    return data->transform.getMinZoom();
}

double Map::getMaxZoom() const {
    return data->transform.getMaxZoom();
}


#pragma mark - Size

uint16_t Map::getWidth() const {
    return data->getTransformState().getWidth();
}

uint16_t Map::getHeight() const {
    return data->getTransformState().getHeight();
}


#pragma mark - Rotation

void Map::rotateBy(double sx, double sy, double ex, double ey, Duration duration) {
    data->transform.rotateBy(sx, sy, ex, ey, duration);
    context->triggerUpdate();
}

void Map::setBearing(double degrees, Duration duration) {
    data->transform.setAngle(-degrees * M_PI / 180, duration);
    context->triggerUpdate();
}

void Map::setBearing(double degrees, double cx, double cy) {
    data->transform.setAngle(-degrees * M_PI / 180, cx, cy);
    context->triggerUpdate();
}

double Map::getBearing() const {
    return -data->transform.getAngle() / M_PI * 180;
}

void Map::resetNorth() {
    data->transform.setAngle(0, std::chrono::milliseconds(500));
    context->triggerUpdate();
}


#pragma mark - Access Token

void Map::setAccessToken(const std::string &token) {
    data->setAccessToken(token);
}

std::string Map::getAccessToken() const {
    return data->getAccessToken();
}


#pragma mark - Projection

void Map::getWorldBoundsMeters(ProjectedMeters& sw, ProjectedMeters& ne) const {
    Projection::getWorldBoundsMeters(sw, ne);
}

void Map::getWorldBoundsLatLng(LatLng& sw, LatLng& ne) const {
    Projection::getWorldBoundsLatLng(sw, ne);
}

double Map::getMetersPerPixelAtLatitude(const double lat, const double zoom) const {
    return Projection::getMetersPerPixelAtLatitude(lat, zoom);
}

const ProjectedMeters Map::projectedMetersForLatLng(const LatLng latLng) const {
    return Projection::projectedMetersForLatLng(latLng);
}

const LatLng Map::latLngForProjectedMeters(const ProjectedMeters projectedMeters) const {
    return Projection::latLngForProjectedMeters(projectedMeters);
}

const vec2<double> Map::pixelForLatLng(const LatLng latLng) const {
    return data->transform.currentState().pixelForLatLng(latLng);
}

const LatLng Map::latLngForPixel(const vec2<double> pixel) const {
    return data->transform.currentState().latLngForPixel(pixel);
}

#pragma mark - Annotations

void Map::setDefaultPointAnnotationSymbol(const std::string& symbol) {
    assert(Environment::currentlyOn(ThreadType::Main));
    context->invokeTask([=] {
        data->annotationManager.setDefaultPointAnnotationSymbol(symbol);
    });
}

double Map::getTopOffsetPixelsForAnnotationSymbol(const std::string& symbol) {
    assert(Environment::currentlyOn(ThreadType::Main));
    return context->invokeSyncTask([&] {
        assert(context->sprite);
        const SpritePosition pos = context->sprite->getSpritePosition(symbol);
        return -pos.height / pos.pixelRatio / 2;
    });
}

uint32_t Map::addPointAnnotation(const LatLng& point, const std::string& symbol) {
    return addPointAnnotations({ point }, { symbol }).front();
}

std::vector<uint32_t> Map::addPointAnnotations(const std::vector<LatLng>& points, const std::vector<std::string>& symbols) {
    assert(Environment::currentlyOn(ThreadType::Main));
    return context->invokeSyncTask([&] {
        auto result = data->annotationManager.addPointAnnotations(points, symbols, *data);
        context->updateAnnotationTiles(result.first);
        return result.second;
    });
}

void Map::removeAnnotation(uint32_t annotation) {
    assert(Environment::currentlyOn(ThreadType::Main));
    removeAnnotations({ annotation });
}

void Map::removeAnnotations(const std::vector<uint32_t>& annotations) {
    assert(Environment::currentlyOn(ThreadType::Main));
    context->invokeTask([=] {
        auto result = data->annotationManager.removeAnnotations(annotations, *data);
        context->updateAnnotationTiles(result);
    });
}

std::vector<uint32_t> Map::getAnnotationsInBounds(const LatLngBounds& bounds) {
    assert(Environment::currentlyOn(ThreadType::Main));
    return context->invokeSyncTask([&] {
        return data->annotationManager.getAnnotationsInBounds(bounds, *data);
    });
}

LatLngBounds Map::getBoundsForAnnotations(const std::vector<uint32_t>& annotations) {
    assert(Environment::currentlyOn(ThreadType::Main));
    return context->invokeSyncTask([&] {
        return data->annotationManager.getBoundsForAnnotations(annotations);
    });
}


#pragma mark - Toggles

void Map::setDebug(bool value) {
    data->setDebug(value);
    context->triggerUpdate(Update::Debug);
}

void Map::toggleDebug() {
    data->toggleDebug();
    context->triggerUpdate(Update::Debug);
}

bool Map::getDebug() const {
    return data->getDebug();
}

void Map::addClass(const std::string& klass) {
    if (data->addClass(klass)) {
        context->triggerUpdate(Update::Classes);
    }
}

void Map::removeClass(const std::string& klass) {
    if (data->removeClass(klass)) {
        context->triggerUpdate(Update::Classes);
    }
}

void Map::setClasses(const std::vector<std::string>& classes) {
    data->setClasses(classes);
    context->triggerUpdate(Update::Classes);
}

bool Map::hasClass(const std::string& klass) const {
    return data->hasClass(klass);
}

std::vector<std::string> Map::getClasses() const {
    return data->getClasses();
}

void Map::setDefaultTransitionDuration(Duration duration) {
    assert(Environment::currentlyOn(ThreadType::Main));

    data->setDefaultTransitionDuration(duration);
    context->triggerUpdate(Update::DefaultTransitionDuration);
}

Duration Map::getDefaultTransitionDuration() {
    assert(Environment::currentlyOn(ThreadType::Main));
    return data->getDefaultTransitionDuration();
}

}
