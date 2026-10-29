// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>

namespace chatterino {

class GeneralPageView;

/// The settings that came from Leafyrino, shown on the JilChat Chatterino page
class LeafyrinoPage : public QWidget
{
    Q_OBJECT

public:
    explicit LeafyrinoPage(GeneralPageView &view);

private:
    void initLayout(GeneralPageView &layout);
};

}  // namespace chatterino
