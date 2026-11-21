#pragma once

#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QWidget>

class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;

namespace chatterino {

class GeneralPageView;
class SignalLabel;

/// The settings that came from Moltorino, shown on the JilChat Chatterino page
class MoltorinoPage : public QWidget
{
    Q_OBJECT

public:
    explicit MoltorinoPage(GeneralPageView &pageView);

    /// Hides the bot badge setup again, when the page is hidden
    void resetOnPageHide();

    /// Reveals the bot badge setup, which is hidden otherwise, and scrolls
    /// to it.
    void showBotBadgeSettings();

private:
    void openAuthDialog();
    void refreshAuthAccounts();
    void updateAuthSummary();
    void updateAuthInstructions(const QString &text, bool isError = false);
    void updateAuthStatus(const QString &text, bool isValid,
                          bool isError = false);
    void revealBotBadgeSettings(bool forceReveal = false);
    void syncBotBadgeVisibility();
    void populateBotBadgeFieldsFromSettings();
    void updateBotBadgeStatus(const QString &text, bool isValid = false,
                              bool isError = false);
    void openBotBadgeAuthorization();
    void verifyBotBadgeConfiguration();

    GeneralPageView *settingsView_{};

    QLabel *authStatusLabel_{};
    QLabel *authInstructionsLabel_{};
    QPushButton *addAuthAccountButton_{};
    QPushButton *refreshAuthAccountsButton_{};
    bool authRefreshInFlight_{false};
    int authRefreshGeneration_{0};

    SignalLabel *logoLabel_{};
    QFrame *botBadgeFrame_{};
    QLabel *botBadgeStatusLabel_{};
    QLabel *botBadgeIdentityLabel_{};
    QLineEdit *botBadgeClientIdEdit_{};
    QLineEdit *botBadgeClientSecretEdit_{};
    QLineEdit *botBadgeSenderEdit_{};
    QPushButton *botBadgeAuthorizeButton_{};
    QPushButton *botBadgeVerifyButton_{};
    int logoClickCount_{0};

    pajlada::Signals::SignalHolder managedConnections_;
    bool botBadgeUnlocked_{false};
    bool botBadgeIsValidating_{false};
    int botBadgeValidationGeneration_{0};

    pajlada::Signals::ScopedConnection chatAutomationRulesConnection_;
};

}  // namespace chatterino
