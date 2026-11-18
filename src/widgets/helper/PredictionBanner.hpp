#pragma once

#include "common/Channel.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/BaseWidget.hpp"
#include "widgets/buttons/SvgButton.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <vector>

namespace chatterino {

class Split;

class PredictionBanner : public BaseWidget
{
    Q_OBJECT

public:
    explicit PredictionBanner(Split *split, QWidget *parent = nullptr);

    void setPrediction(
        const std::optional<TwitchChannel::PredictionEvent> &prediction,
        TwitchChannel *channel);

    bool hasPrediction() const;

    /// Clears the "dismissed" state so a previously dismissed prediction can be
    /// shown again.
    void clearDismissState();

    void setToggleButtonVisible(bool visible);

    pajlada::Signals::NoArgSignal toggleBannerRequested;

    pajlada::Signals::NoArgSignal dismissed;

protected:
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QDateTime predictionEndsAt() const;
    void updateTimer();
    void updateLayout();
    void updateLabelStyles();
    void openPredictionDialog();
    /// Shows Pay out and Refund buttons to mods while the prediction is open
    void updateModButtons();
    /// First click arms the button, a second click within a few seconds runs
    /// the command, since paying out or refunding can't be undone
    void armOrRun(QPushButton *button, const QString &label,
                  const QString &command);
    QString dismissalKey(
        const TwitchChannel::PredictionEvent &prediction) const;

    SvgButton *icon_{};
    QLabel *metadataLabel_{};
    QLabel *titleLabel_{};
    QLabel *leftOutcomeLabel_{};
    QLabel *rightOutcomeLabel_{};
    QWidget *outcomeBar_{};
    QLabel *timerLabel_{};
    SvgButton *closeButton_{};
    SvgButton *toggleButton_{};
    QVBoxLayout *rootLayout_{};
    QHBoxLayout *topLayout_{};
    QVBoxLayout *contentLayout_{};
    QHBoxLayout *outcomeRowLayout_{};
    QWidget *modButtons_{};
    QHBoxLayout *modButtonsLayout_{};
    /// Which prediction and state the buttons were made for
    QString modButtonsKey_;
    QPushButton *armedButton_{};
    Split *split_{};

    int headerFontSize_{11};
    int titleFontSize_{13};
    int outcomeFontSize_{12};

    TwitchChannel *twitchChannel_{};
    std::optional<TwitchChannel::PredictionEvent> prediction_;
    QTimer *updateTimer_{};
    QString dismissedPredictionKey_;

    std::vector<double> previousFractions_;
    std::vector<double> targetFractions_;
    std::vector<double> currentFractions_;
    QVariantAnimation *anim_{};

    std::vector<pajlada::Signals::ScopedConnection> managedConnections_;
};

}  // namespace chatterino
