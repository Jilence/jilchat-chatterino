#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

/// Frame timing of a QQuickWindow: frame intervals (frameSwapped) and
/// render-thread time (beforeSynchronizing -> afterRendering, i.e. without
/// waiting for vsync).
class FrameStats : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(double fps READ fps NOTIFY updated)
    Q_PROPERTY(double frameMs READ frameMs NOTIFY updated)
    Q_PROPERTY(double worstMs READ worstMs NOTIFY updated)
    Q_PROPERTY(double renderMs READ renderMs NOTIFY updated)
    Q_PROPERTY(double gpuMs READ gpuMs NOTIFY updated)
    Q_PROPERTY(QString api READ api NOTIFY updated)

public:
    explicit FrameStats(QObject *parent = nullptr);

    QQuickWindow *window() const { return window_; }
    void setWindow(QQuickWindow *window);
    double fps() const { return fps_; }
    double frameMs() const { return frameMs_; }
    double worstMs() const { return worstMs_; }
    double renderMs() const { return renderMs_; }
    double gpuMs() const { return gpuMs_; }
    QString api() const { return api_; }

    /// Starts a new benchmark window.
    Q_INVOKABLE void resetTotals();
    /// Benchmark result since the last resetTotals().
    Q_INVOKABLE QString summary() const;

signals:
    void windowChanged();
    void updated();

private:
    void tick();

    QPointer<QQuickWindow> window_;
    QElapsedTimer clock_;
    QTimer timer_;

    QMutex mutex_;  // guards the fields written on the render thread
    qint64 lastFrameNs_ = -1;
    qint64 syncStartNs_ = -1;
    QList<double> intervals_;
    QList<double> renderTimes_;
    QList<double> gpuTimes_;

    qint64 lastTickNs_ = 0;
    QList<double> totalIntervals_;
    QList<double> totalRender_;
    QList<double> totalGpu_;
    double gpuMs_ = 0;
    double fps_ = 0;
    double frameMs_ = 0;
    double worstMs_ = 0;
    double renderMs_ = 0;
    QString api_;
};
