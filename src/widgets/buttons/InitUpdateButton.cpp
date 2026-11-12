// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/InitUpdateButton.hpp"

#include "Application.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/UpdatePrompt.hpp"

namespace chatterino {

void initUpdateButton(SvgButton &button, const std::function<void()> &relayout,
                      pajlada::Signals::SignalHolder &signalHolder)
{
    button.hide();

    // show the update window when clicking the button
    QObject::connect(&button, &Button::leftClicked, [] {
        UpdatePrompt::showPrompt();
    });

    // update image when state changes
    auto updateChange = [&button, relayout](auto) {
        button.setVisible(getApp()->getUpdates().shouldShowUpdateButton());

        const bool isError = getApp()->getUpdates().isError();
        button.setSource({
            .dark = isError ? ":/buttons/updateError-darkMode.svg"
                            : ":/buttons/update-darkMode.svg",
            .light = isError ? ":/buttons/updateError-darkMode.svg"
                             : ":/buttons/update-lightMode.svg",
        });

        relayout();
    };

    updateChange(getApp()->getUpdates().getStatus());

    signalHolder.managedConnect(getApp()->getUpdates().statusUpdated,
                                [updateChange](auto status) {
                                    updateChange(status);
                                });
}

}  // namespace chatterino
