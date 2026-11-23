#pragma once

#include <QCanvasPainterWidget>
#include <QRegion>

#include <functional>
#include <memory>

class QPainter;
namespace chatterino {
// GPU presentation for the existing message layout and raster text caches.
class CanvasChatSurface final : public QCanvasPainterWidget
{
public:
    CanvasChatSurface(QWidget *parent, std::function<void(QPainter &)> draw);
    CanvasChatSurface(QWidget *parent,
                      std::function<void(QPainter &, const QRegion &)> draw);
    ~CanvasChatSurface() override;
    void repaintRegion(const QRegion &region);

protected:
    void render(QRhiCommandBuffer *commands) override;
    void prePaint(QCanvasPainter *painter) override;
    void paint(QCanvasPainter *painter) override;
    void graphicsResourcesInvalidated() override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}  // namespace chatterino
