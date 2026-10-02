// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/UsercardRolesView.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "util/Clipboard.hpp"
#include "widgets/Label.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <optional>

namespace chatterino {

using namespace literals;

namespace {

constexpr int TIMEOUT_MS = 15000;
constexpr int PAGE_SIZE = 50;
constexpr int LOGIN_ROLE = Qt::UserRole;

struct Role {
    /// The name in roles.tv's API.
    QString key;
    QString title;
};

const std::array<Role, 4> ROLES{{
    {u"moderators"_s, u"Mods"_s},
    {u"vips"_s, u"VIPs"_s},
    {u"founders"_s, u"Founders"_s},
    {u"artists"_s, u"Artists"_s},
}};

const QString API_URL = u"https://roles.tv/api"_s;

/// roles.tv's timestamps are UTC, like "2025-01-28 00:53:15.12345".
QString formatSince(const QString &grantedAt)
{
    auto time =
        QDateTime::fromString(grantedAt.left(19), u"yyyy-MM-dd HH:mm:ss"_s);
    if (!time.isValid())
    {
        return {};
    }
    time.setTimeZone(QTimeZone::UTC);
    return u"Since %1"_s.arg(
        QLocale().toString(time.toLocalTime().date(), u"MMM d, yyyy"_s));
}

}  // namespace

UsercardRolesView::UsercardRolesView(QWidget *parent)
    : BaseWidget(parent)
{
    // Frameless popups drag the window from widgets without mouse tracking.
    this->setMouseTracking(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *topRow = new QHBoxLayout();
    this->scope_ = new QComboBox(this);
    this->scope_->addItem("Their roles", u"user"_s);
    this->scope_->addItem("Their channel", u"channel"_s);
    this->scope_->setToolTip(
        "Switch between roles this user holds and roles in their channel");
    auto *openWeb = new QPushButton("Open on web", this);
    topRow->addWidget(this->scope_);
    topRow->addStretch(1);
    topRow->addWidget(openWeb);
    layout->addLayout(topRow);

    auto *tabRow = new QHBoxLayout();
    tabRow->setSpacing(2);
    for (size_t i = 0; i < ROLE_COUNT; ++i)
    {
        auto *tab = new QPushButton(ROLES.at(i).title, this);
        tab->setCheckable(true);
        tab->setAutoExclusive(true);
        tab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QObject::connect(tab, &QPushButton::clicked, this, [this, i] {
            this->selectRole(i);
        });
        tabRow->addWidget(tab, 1);
        this->tabs_.at(i) = tab;
    }
    layout->addLayout(tabRow);

    this->list_ = new QListWidget(this);
    this->list_->setAlternatingRowColors(true);
    this->list_->setSelectionMode(QAbstractItemView::NoSelection);
    this->list_->setFocusPolicy(Qt::NoFocus);
    this->list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(this->list_, 1);

    this->status_ = new Label(this);
    this->status_->setCentered(true);
    this->status_->setSizePolicy(QSizePolicy::Expanding,
                                 QSizePolicy::Expanding);
    this->status_->hide();
    layout->addWidget(this->status_, 1);

    this->loadMore_ = new QPushButton("Load more", this);
    this->loadMore_->hide();
    layout->addWidget(this->loadMore_);

    QObject::connect(this->scope_, &QComboBox::currentIndexChanged, this,
                     [this] {
                         this->loadSummary();
                     });
    QObject::connect(openWeb, &QPushButton::clicked, this, [this] {
        if (this->login_.isEmpty())
        {
            return;
        }
        QDesktopServices::openUrl(QUrl(u"https://roles.tv/%1/%2"_s.arg(
            this->scope() == u"channel" ? u"c"_s : u"u"_s,
            QString::fromUtf8(QUrl::toPercentEncoding(this->login_)))));
    });
    QObject::connect(this->loadMore_, &QPushButton::clicked, this, [this] {
        this->loadPage();
    });
    QObject::connect(this->list_, &QListWidget::customContextMenuRequested,
                     this, &UsercardRolesView::showContextMenu);
}

QString UsercardRolesView::scope() const
{
    return this->scope_->currentData().toString();
}

void UsercardRolesView::setTarget(const QString &userId, const QString &login)
{
    if (this->userId_ == userId && this->login_ == login)
    {
        return;
    }
    this->userId_ = userId;
    this->login_ = login;
    this->loadSummary();
}

void UsercardRolesView::loadSummary()
{
    const auto generation = ++this->generation_;
    this->list_->clear();
    this->cursor_.clear();
    this->loadMore_->hide();
    for (auto *tab : this->tabs_)
    {
        tab->hide();
    }
    if (this->userId_.isEmpty())
    {
        this->setStatus("This Twitch user is not ready yet.");
        return;
    }
    this->setStatus("Loading roles...");

    const QPointer<UsercardRolesView> self(this);
    NetworkRequest(u"%1/%2/id/%3"_s.arg(API_URL, this->scope(), this->userId_))
        .timeout(TIMEOUT_MS)
        .onSuccess([self, generation](const NetworkResult &result) {
            if (!self || self->generation_ != generation)
            {
                return;
            }
            const auto roles = result.parseJson()
                                   .value("data")
                                   .toObject()
                                   .value("roles")
                                   .toObject();
            std::optional<size_t> first;
            for (size_t i = 0; i < ROLE_COUNT; ++i)
            {
                const auto total = roles.value(ROLES.at(i).key).toInt();
                self->totals_.at(i) = total;
                self->tabs_.at(i)->setText(
                    u"%1 %2"_s.arg(ROLES.at(i).title).arg(total));
                // Only the kinds of roles there are any of.
                self->tabs_.at(i)->setVisible(total > 0);
                if (total > 0 && !first)
                {
                    first = i;
                }
            }
            if (!first)
            {
                self->setStatus(
                    self->scope() == u"channel"
                        ? u"No tracked roles found in this channel."_s
                        : u"No tracked roles found for this user."_s);
                return;
            }
            self->selectRole(*first);
        })
        .onError([self, generation](const NetworkResult &result) {
            if (!self || self->generation_ != generation)
            {
                return;
            }
            // roles.tv answers 404 for users it doesn't track.
            self->setStatus(result.status() == 404
                                ? u"Roles are unavailable for this user."_s
                                : u"Roles couldn't be loaded: %1"_s.arg(
                                      result.formatError()));
        })
        .execute();
}

void UsercardRolesView::selectRole(size_t role)
{
    this->role_ = role;
    this->tabs_.at(role)->setChecked(true);
    this->list_->clear();
    this->cursor_.clear();
    this->loadPage();
}

void UsercardRolesView::loadPage()
{
    const auto generation = ++this->generation_;
    this->loadMore_->setEnabled(false);
    this->loadMore_->setText("Loading more...");
    if (this->list_->count() == 0)
    {
        this->setStatus(this->scope() == u"channel" ? u"Loading users..."_s
                                                    : u"Loading channels..."_s);
    }

    QUrl url(u"%1/stats/%2/%3/id/%4"_s.arg(
        API_URL, this->scope(), ROLES.at(this->role_).key, this->userId_));
    QUrlQuery query;
    query.addQueryItem(u"per_page"_s, QString::number(PAGE_SIZE));
    if (!this->cursor_.isEmpty())
    {
        query.addQueryItem(
            u"after"_s,
            QString::fromUtf8(QUrl::toPercentEncoding(this->cursor_)));
    }
    url.setQuery(query);

    const QPointer<UsercardRolesView> self(this);
    NetworkRequest(url)
        .timeout(TIMEOUT_MS)
        .onSuccess([self, generation](const NetworkResult &result) {
            if (!self || self->generation_ != generation)
            {
                return;
            }
            const auto root = result.parseJson();
            self->cursor_ = root.value("cursor").toString();
            self->addEntries(root.value("data").toArray());
        })
        .onError([self, generation](const NetworkResult &result) {
            if (!self || self->generation_ != generation)
            {
                return;
            }
            if (self->list_->count() == 0)
            {
                self->setStatus(u"Roles couldn't be loaded: %1"_s.arg(
                    result.formatError()));
            }
            self->loadMore_->setEnabled(true);
            self->loadMore_->setText("Load more");
        })
        .execute();
}

void UsercardRolesView::addEntries(const QJsonArray &entries)
{
    for (const auto &value : entries)
    {
        const auto entry = value.toObject();
        // Roles that were taken away again stay in the list of roles.tv.
        if (!entry.value("active").toBool(true))
        {
            continue;
        }
        const auto login = entry.value("login").toString();
        const auto name = entry.value("displayName").toString(login);
        const auto since = formatSince(entry.value("grantedAt").toString());
        const auto status = entry.value("isPartner").toBool() ? u"Partner"_s
                            : entry.value("isAffiliate").toBool()
                                ? u"Affiliate"_s
                                : QString();

        auto *row = new QWidget(this->list_);
        row->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 4, 8, 4);
        auto *nameLabel = new QLabel(
            u"<b>%1</b><br><span style=\"color: #999;\">%2</span>"_s.arg(
                name.toHtmlEscaped(), since.toHtmlEscaped()),
            row);
        nameLabel->setTextFormat(Qt::RichText);
        auto *statusLabel = new QLabel(status, row);
        statusLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
        rowLayout->addWidget(nameLabel, 1);
        rowLayout->addWidget(statusLabel);

        auto *item = new QListWidgetItem(this->list_);
        item->setData(LOGIN_ROLE, login);
        item->setSizeHint(row->sizeHint());
        this->list_->setItemWidget(item, row);
    }

    const bool hasMore = !this->cursor_.isEmpty();
    this->loadMore_->setVisible(hasMore);
    this->loadMore_->setEnabled(true);
    this->loadMore_->setText(u"Load more (%1 of %2)"_s.arg(this->list_->count())
                                 .arg(this->totals_.at(this->role_)));
    this->setStatus(this->list_->count() == 0 ? u"No active roles found."_s
                                              : QString());
}

void UsercardRolesView::setStatus(const QString &text)
{
    this->status_->setText(text);
    this->status_->setVisible(!text.isEmpty());
    this->list_->setVisible(text.isEmpty());
}

void UsercardRolesView::showContextMenu(const QPoint &pos)
{
    const auto *item = this->list_->itemAt(pos);
    if (item == nullptr)
    {
        return;
    }
    const auto login = item->data(LOGIN_ROLE).toString();
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction("Copy username", [login] {
        crossPlatformCopy(login);
    });
    menu->addAction("Open channel on Twitch", [login] {
        QDesktopServices::openUrl(QUrl(u"https://www.twitch.tv/"_s + login));
    });
    menu->popup(this->list_->viewport()->mapToGlobal(pos));
}

}  // namespace chatterino
