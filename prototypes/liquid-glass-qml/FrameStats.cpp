#include "FrameStats.hpp"

#include <QSGRendererInterface>
#include <rhi/qrhi.h>

#include <algorithm>
#include <numeric>

namespace {

double average(const QList<double> &v)
{
    return v.isEmpty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
}

double percentile(QList<double> v, double p)
{
    if (v.isEmpty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v.at(std::min<qsizetype>(v.size() - 1, qsizetype(p * double(v.size()))));
}

QString apiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("D3D11");
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("D3D12");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("Vulkan");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("OpenGL");
    case QSGRendererInterface::Metal:
        return QStringLiteral("Metal");
    case QSGRendererInterface::Software:
        return QStringLiteral("Software");
    default:
        return QStringLiteral("?");
    }
}

}  // namespace

FrameStats::FrameStats(QObject *parent)
    : QObject(parent)
{
    clock_.start();
    lastTickNs_ = clock_.nsecsElapsed();
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, &FrameStats::tick);
    timer_.start();
}

void FrameStats::setWindow(QQuickWindow *window)
{
    if (window_ == window)
        return;
    if (window_)
        disconnect(window_, nullptr, this, nullptr);
    window_ = window;
    if (window) {
        // These fire on the render thread.
        connect(window, &QQuickWindow::beforeSynchronizing, this, [this] {
            QMutexLocker lock(&mutex_);
            syncStartNs_ = clock_.nsecsElapsed();
        }, Qt::DirectConnection);
        connect(window, &QQuickWindow::afterRendering, this, [this] {
            // GPU time of the last completed frame; needs timestamps enabled
            // in the window's graphics configuration (see main.cpp).
            double gpu = 0;
            if (window_) {
                if (auto *sc = window_->swapChain())
                    gpu = sc->currentFrameCommandBuffer()->lastCompletedGpuTime() * 1000.0;
            }
            QMutexLocker lock(&mutex_);
            if (syncStartNs_ >= 0)
                renderTimes_.append(double(clock_.nsecsElapsed() - syncStartNs_) / 1e6);
            syncStartNs_ = -1;
            if (gpu > 0)
                gpuTimes_.append(gpu);
        }, Qt::DirectConnection);
        connect(window, &QQuickWindow::frameSwapped, this, [this] {
            const qint64 now = clock_.nsecsElapsed();
            QMutexLocker lock(&mutex_);
            if (lastFrameNs_ >= 0)
                intervals_.append(double(now - lastFrameNs_) / 1e6);
            lastFrameNs_ = now;
        }, Qt::DirectConnection);
    }
    emit windowChanged();
}

void FrameStats::tick()
{
    QList<double> iv, rt, gt;
    {
        QMutexLocker lock(&mutex_);
        iv.swap(intervals_);
        rt.swap(renderTimes_);
        gt.swap(gpuTimes_);
    }
    totalIntervals_ += iv;
    totalRender_ += rt;
    totalGpu_ += gt;
    gpuMs_ = average(gt);

    const qint64 now = clock_.nsecsElapsed();
    const double dt = double(now - lastTickNs_) / 1e9;
    lastTickNs_ = now;

    fps_ = dt > 0 ? double(iv.size()) / dt : 0;
    frameMs_ = average(iv);
    worstMs_ = iv.isEmpty() ? 0 : *std::max_element(iv.begin(), iv.end());
    renderMs_ = average(rt);
    if (api_.isEmpty() && window_ && window_->isSceneGraphInitialized())
        api_ = apiName(window_->rendererInterface()->graphicsApi());
    emit updated();
}

void FrameStats::resetTotals()
{
    QMutexLocker lock(&mutex_);
    intervals_.clear();
    renderTimes_.clear();
    gpuTimes_.clear();
    totalIntervals_.clear();
    totalRender_.clear();
    totalGpu_.clear();
}

QString FrameStats::summary() const
{
    const auto &iv = totalIntervals_;
    const double sum = std::accumulate(iv.begin(), iv.end(), 0.0);
    const double avgFps = sum > 0 ? double(iv.size()) * 1000.0 / sum : 0;
    const double p50 = percentile(iv, 0.50);
    const double p99 = percentile(iv, 0.99);
    const double worst = iv.isEmpty() ? 0 : *std::max_element(iv.begin(), iv.end());
    return QStringLiteral(
               "api=%1 frames=%2 avg_fps=%3 frame_p50=%4ms frame_p99=%5ms worst=%6ms "
               "render_avg=%7ms render_p99=%8ms gpu_avg=%9ms gpu_p99=%10ms")
        .arg(api_)
        .arg(iv.size())
        .arg(avgFps, 0, 'f', 1)
        .arg(p50, 0, 'f', 2)
        .arg(p99, 0, 'f', 2)
        .arg(worst, 0, 'f', 2)
        .arg(average(totalRender_), 0, 'f', 3)
        .arg(percentile(totalRender_, 0.99), 0, 'f', 3)
        .arg(average(totalGpu_), 0, 'f', 3)
        .arg(percentile(totalGpu_, 0.99), 0, 'f', 3);
}
