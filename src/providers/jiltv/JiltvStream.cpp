// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/jiltv/JiltvStream.hpp"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace chatterino {

namespace {

constexpr int MAX_BACKOFF_MS = 30000;
constexpr int WATCHDOG_MS = 60000;

}  // namespace

JiltvStream::JiltvStream(QNetworkAccessManager *network, QUrl url,
                         Handler handler)
    : network_(network)
    , url_(std::move(url))
    , handler_(std::move(handler))
{
    this->retry_.setSingleShot(true);
    QObject::connect(&this->retry_, &QTimer::timeout, &this->context_, [this] {
        this->connect();
    });
    this->watchdog_.setSingleShot(true);
    this->watchdog_.setInterval(WATCHDOG_MS);
    QObject::connect(&this->watchdog_, &QTimer::timeout, &this->context_,
                     [this] {
                         if (this->reply_)
                         {
                             this->reply_->abort();  // reconnects in onFinished
                         }
                     });
    this->connect();
}

JiltvStream::~JiltvStream()
{
    this->stopped_ = true;
    this->retry_.stop();
    this->watchdog_.stop();
    if (this->reply_)
    {
        this->reply_->disconnect(&this->context_);
        this->reply_->abort();
        this->reply_->deleteLater();
    }
}

const QUrl &JiltvStream::url() const
{
    return this->url_;
}

void JiltvStream::connect()
{
    if (this->stopped_)
    {
        return;
    }
    QNetworkRequest req(this->url_);
    req.setRawHeader("Accept", "text/event-stream");
    req.setRawHeader("Cache-Control", "no-cache");
    if (!this->lastId_.isEmpty())
    {
        req.setRawHeader("Last-Event-ID", this->lastId_.toUtf8());
    }
    // The stream stays open; Qt's transfer timeout would cut it.
    req.setTransferTimeout(0);
    this->buffer_.clear();
    this->event_.clear();
    this->data_.clear();
    this->reply_ = this->network_->get(req);
    QObject::connect(this->reply_, &QNetworkReply::readyRead, &this->context_,
                     [this] {
                         this->onData();
                     });
    QObject::connect(this->reply_, &QNetworkReply::finished, &this->context_,
                     [this] {
                         this->onFinished();
                     });
    this->watchdog_.start();
}

void JiltvStream::onData()
{
    if (!this->reply_)
    {
        return;
    }
    this->watchdog_.start();
    if (this->reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute)
            .toInt() == 200)
    {
        this->backoffMs_ = 1000;  // up and running
    }
    this->buffer_ += this->reply_->readAll();
    qsizetype nl = 0;
    while ((nl = this->buffer_.indexOf('\n')) >= 0)
    {
        QByteArray line = this->buffer_.left(nl);
        this->buffer_.remove(0, nl + 1);
        if (line.endsWith('\r'))
        {
            line.chop(1);
        }
        if (line.isEmpty())
        {
            this->dispatch();
        }
        else if (line.startsWith(':'))
        {
            // comment, e.g. the heartbeat
        }
        else if (line.startsWith("id:"))
        {
            this->lastId_ = QString::fromUtf8(line.mid(3)).trimmed();
        }
        else if (line.startsWith("event:"))
        {
            this->event_ = QString::fromUtf8(line.mid(6)).trimmed();
        }
        else if (line.startsWith("data:"))
        {
            if (!this->data_.isEmpty())
            {
                this->data_ += '\n';
            }
            this->data_ += line.mid(line.startsWith("data: ") ? 6 : 5);
        }
    }
}

void JiltvStream::dispatch()
{
    if (!this->event_.isEmpty() && this->handler_)
    {
        const auto doc = QJsonDocument::fromJson(this->data_);
        this->handler_(this->event_, doc.object());
    }
    this->event_.clear();
    this->data_.clear();
}

void JiltvStream::onFinished()
{
    this->watchdog_.stop();
    if (this->reply_)
    {
        this->reply_->deleteLater();
        this->reply_ = nullptr;
    }
    if (this->stopped_)
    {
        return;
    }
    this->retry_.start(this->backoffMs_);
    this->backoffMs_ = std::min(this->backoffMs_ * 2, MAX_BACKOFF_MS);
}

}  // namespace chatterino
