/*
 * nheko Copyright (C) 2017  Konstantinos Sideris <siderisk@auth.gr>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QApplication>
#include <QFileInfo>
#include <QTimer>

#include "FloatingButton.h"
#include "RoomMessages.h"
#include "ScrollBar.h"
#include "Sync.h"

#include "timeline/TimelineView.h"
#include "timeline/widgets/AudioItem.h"
#include "timeline/widgets/FileItem.h"
#include "timeline/widgets/ImageItem.h"
#include "timeline/widgets/VideoItem.h"

namespace events = matrix::events;
namespace msgs   = matrix::events::messages;

static bool
isRedactedEvent(const QJsonObject &event)
{
        if (event.contains("redacted_because"))
                return true;

        if (event.contains("unsigned") &&
            event.value("unsigned").toObject().contains("redacted_because"))
                return true;

        return false;
}

TimelineView::TimelineView(const Timeline &timeline,
                           QSharedPointer<MatrixClient> client,
                           const QString &room_id,
                           QWidget *parent)
  : QWidget(parent)
  , room_id_{room_id}
  , client_{client}
{
        init();
        addEvents(timeline);
}

TimelineView::TimelineView(QSharedPointer<MatrixClient> client,
                           const QString &room_id,
                           QWidget *parent)
  : QWidget(parent)
  , room_id_{room_id}
  , client_{client}
{
        init();
}

void
TimelineView::sliderRangeChanged(int min, int max)
{
        Q_UNUSED(min);

        if (!scroll_area_->verticalScrollBar()->isVisible()) {
                scroll_area_->verticalScrollBar()->setValue(max);
                return;
        }

        // If the scrollbar is close to the bottom and a new message
        // is added we move the scrollbar.
        if (max - scroll_area_->verticalScrollBar()->value() < SCROLL_BAR_GAP) {
                scroll_area_->verticalScrollBar()->setValue(max);
                return;
        }

        int currentHeight = scroll_widget_->size().height();
        int diff          = currentHeight - oldHeight_;
        int newPosition   = oldPosition_ + diff;

        // Keep the scroll bar to the bottom if it hasn't been activated yet.
        if (oldPosition_ == 0 && !scroll_area_->verticalScrollBar()->isVisible())
                newPosition = max;

        if (lastMessageDirection_ == TimelineDirection::Top)
                scroll_area_->verticalScrollBar()->setValue(newPosition);
}

void
TimelineView::fetchHistory()
{
        bool hasEnoughMessages = scroll_area_->verticalScrollBar()->isVisible();

        if (!hasEnoughMessages && !isTimelineFinished) {
                isPaginationInProgress_ = true;
                client_->messages(room_id_, prev_batch_token_);
                paginationTimer_->start(500);
                return;
        }

        paginationTimer_->stop();
}

void
TimelineView::scrollDown()
{
        int current = scroll_area_->verticalScrollBar()->value();
        int max     = scroll_area_->verticalScrollBar()->maximum();

        // The first time we enter the room move the scroll bar to the bottom.
        if (!isInitialized) {
                scroll_area_->verticalScrollBar()->setValue(max);
                isInitialized = true;
                return;
        }

        // If the gap is small enough move the scroll bar down. e.g when a new
        // message appears.
        if (max - current < SCROLL_BAR_GAP)
                scroll_area_->verticalScrollBar()->setValue(max);
}

void
TimelineView::sliderMoved(int position)
{
        if (!scroll_area_->verticalScrollBar()->isVisible())
                return;

        const int maxScroll     = scroll_area_->verticalScrollBar()->maximum();
        const int currentScroll = scroll_area_->verticalScrollBar()->value();

        if (maxScroll - currentScroll > SCROLL_BAR_GAP) {
                scrollDownBtn_->show();
                scrollDownBtn_->raise();
        } else {
                scrollDownBtn_->hide();
        }

        // The scrollbar is high enough so we can start retrieving old events.
        if (position < SCROLL_BAR_GAP) {
                if (isTimelineFinished)
                        return;

                // Prevent user from moving up when there is pagination in
                // progress.
                // TODO: Keep a map of the event ids to filter out duplicates.
                if (isPaginationInProgress_)
                        return;

                isPaginationInProgress_ = true;

                // FIXME: Maybe move this to TimelineViewManager to remove the
                // extra calls?
                client_->messages(room_id_, prev_batch_token_);
        }
}

void
TimelineView::addBackwardsEvents(const QString &room_id, const RoomMessages &msgs)
{
        if (room_id_ != room_id)
                return;

        if (msgs.chunk().count() == 0) {
                isTimelineFinished = true;
                return;
        }

        isTimelineFinished = false;
        QList<TimelineItem *> items;

        // Reset the sender of the first message in the timeline
        // cause we're about to insert a new one.
        firstSender_.clear();

        // Parse in reverse order to determine where we should not show sender's
        // name.
        auto ii = msgs.chunk().size();
        while (ii != 0) {
                --ii;

                TimelineItem *item =
                  parseMessageEvent(msgs.chunk().at(ii).toObject(), TimelineDirection::Top);

                if (item != nullptr)
                        items.push_back(item);
        }

        // Reverse again to render them.
        std::reverse(items.begin(), items.end());

        oldPosition_ = scroll_area_->verticalScrollBar()->value();
        oldHeight_   = scroll_widget_->size().height();

        for (const auto &item : items)
                addTimelineItem(item, TimelineDirection::Top);

        lastMessageDirection_ = TimelineDirection::Top;

        QApplication::processEvents();

        prev_batch_token_       = msgs.end();
        isPaginationInProgress_ = false;

        // Exclude the top stretch.
        if (!msgs.chunk().isEmpty() && scroll_layout_->count() > 1)
                notifyForLastEvent();

        // If this batch is the first being rendered (i.e the first and the last
        // events originate from this batch), set the last sender.
        if (lastSender_.isEmpty() && !items.isEmpty())
                lastSender_ = items.constFirst()->descriptionMessage().userid;
}

TimelineItem *
TimelineView::parseMessageEvent(const QJsonObject &event, TimelineDirection direction)
{
        events::EventType ty = events::extractEventType(event);

        if (ty == events::EventType::RoomMessage) {
                events::MessageEventType msg_type = events::extractMessageEventType(event);

                using Audio  = events::MessageEvent<msgs::Audio>;
                using Emote  = events::MessageEvent<msgs::Emote>;
                using File   = events::MessageEvent<msgs::File>;
                using Image  = events::MessageEvent<msgs::Image>;
                using Notice = events::MessageEvent<msgs::Notice>;
                using Text   = events::MessageEvent<msgs::Text>;
                using Video  = events::MessageEvent<msgs::Video>;

                if (msg_type == events::MessageEventType::Audio) {
                        return processMessageEvent<Audio, AudioItem>(event, direction);
                } else if (msg_type == events::MessageEventType::Emote) {
                        return processMessageEvent<Emote>(event, direction);
                } else if (msg_type == events::MessageEventType::File) {
                        return processMessageEvent<File, FileItem>(event, direction);
                } else if (msg_type == events::MessageEventType::Image) {
                        return processMessageEvent<Image, ImageItem>(event, direction);
                } else if (msg_type == events::MessageEventType::Notice) {
                        return processMessageEvent<Notice>(event, direction);
                } else if (msg_type == events::MessageEventType::Text) {
                        return processMessageEvent<Text>(event, direction);
                } else if (msg_type == events::MessageEventType::Video) {
                        return processMessageEvent<Video, VideoItem>(event, direction);
                } else if (msg_type == events::MessageEventType::Unknown) {
                        // TODO Handle redacted messages.
                        // Silenced for now.
                        if (!isRedactedEvent(event))
                                qWarning() << "Unknown message type" << event;

                        return nullptr;
                }
        }

        return nullptr;
}

int
TimelineView::addEvents(const Timeline &timeline)
{
        int message_count = 0;

        QSettings settings;
        QString localUser = settings.value("auth/user_id").toString();

        for (const auto &event : timeline.events()) {
                TimelineItem *item = parseMessageEvent(event.toObject(), TimelineDirection::Bottom);

                if (item != nullptr) {
                        addTimelineItem(item, TimelineDirection::Bottom);

                        if (localUser != event.toObject().value("sender").toString())
                                message_count += 1;
                }
        }

        lastMessageDirection_ = TimelineDirection::Bottom;

        QApplication::processEvents();

        if (isInitialSync) {
                prev_batch_token_ = timeline.previousBatch();
                isInitialSync     = false;
        }

        // Exclude the top stretch.
        if (!timeline.events().isEmpty() && scroll_layout_->count() > 1)
                notifyForLastEvent();

        if (isActiveWindow() && isVisible() && timeline.events().size() > 0)
                readLastEvent();

        return message_count;
}

void
TimelineView::init()
{
        QSettings settings;
        local_user_ = settings.value("auth/user_id").toString();

        QIcon icon;
        icon.addFile(":/icons/icons/ui/angle-arrow-down.png");
        scrollDownBtn_ = new FloatingButton(icon, this);
        scrollDownBtn_->setBackgroundColor(QColor("#F5F5F5"));
        scrollDownBtn_->setForegroundColor(QColor("black"));
        scrollDownBtn_->hide();

        connect(scrollDownBtn_, &QPushButton::clicked, this, [=]() {
                const int max = scroll_area_->verticalScrollBar()->maximum();
                scroll_area_->verticalScrollBar()->setValue(max);
        });
        top_layout_ = new QVBoxLayout(this);
        top_layout_->setSpacing(0);
        top_layout_->setMargin(0);

        scroll_area_ = new QScrollArea(this);
        scroll_area_->setWidgetResizable(true);
        scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        scrollbar_ = new ScrollBar(scroll_area_);
        scroll_area_->setVerticalScrollBar(scrollbar_);

        scroll_widget_ = new QWidget(this);

        scroll_layout_ = new QVBoxLayout(scroll_widget_);
        scroll_layout_->setContentsMargins(15, 0, 15, 15);
        scroll_layout_->addStretch(1);
        scroll_layout_->setSpacing(0);
        scroll_layout_->setObjectName("timelinescrollarea");

        scroll_area_->setWidget(scroll_widget_);

        top_layout_->addWidget(scroll_area_);

        setLayout(top_layout_);

        paginationTimer_ = new QTimer(this);
        connect(paginationTimer_, &QTimer::timeout, this, &TimelineView::fetchHistory);

        connect(client_.data(),
                &MatrixClient::messagesRetrieved,
                this,
                &TimelineView::addBackwardsEvents);

        connect(scroll_area_->verticalScrollBar(),
                SIGNAL(valueChanged(int)),
                this,
                SLOT(sliderMoved(int)));
        connect(scroll_area_->verticalScrollBar(),
                SIGNAL(rangeChanged(int, int)),
                this,
                SLOT(sliderRangeChanged(int, int)));
}

void
TimelineView::updateLastSender(const QString &user_id, TimelineDirection direction)
{
        if (direction == TimelineDirection::Bottom)
                lastSender_ = user_id;
        else
                firstSender_ = user_id;
}

bool
TimelineView::isSenderRendered(const QString &user_id, TimelineDirection direction)
{
        if (direction == TimelineDirection::Bottom)
                return lastSender_ != user_id;
        else
                return firstSender_ != user_id;
}

void
TimelineView::addTimelineItem(TimelineItem *item, TimelineDirection direction)
{
        if (direction == TimelineDirection::Bottom)
                scroll_layout_->addWidget(item);
        else
                scroll_layout_->insertWidget(1, item);
}

void
TimelineView::updatePendingMessage(int txn_id, QString event_id)
{
        if (pending_msgs_.head().txn_id == txn_id) { // We haven't received it yet
                auto msg     = pending_msgs_.dequeue();
                msg.event_id = event_id;
                pending_sent_msgs_.append(msg);
        }
        sendNextPendingMessage();
}

void
TimelineView::addUserMessage(matrix::events::MessageEventType ty, const QString &body)
{
        QSettings settings;
        auto user_id     = settings.value("auth/user_id").toString();
        auto with_sender = lastSender_ != user_id;

        TimelineItem *view_item = new TimelineItem(ty, user_id, body, with_sender, scroll_widget_);
        scroll_layout_->addWidget(view_item);

        lastMessageDirection_ = TimelineDirection::Bottom;

        QApplication::processEvents();

        lastSender_ = user_id;

        int txn_id = client_->incrementTransactionId();
        PendingMessage message(ty, txn_id, body, "", "", view_item);
        handleNewUserMessage(message);
}

void
TimelineView::handleNewUserMessage(PendingMessage msg)
{
        pending_msgs_.enqueue(msg);
        if (pending_msgs_.size() == 1 && pending_sent_msgs_.size() == 0)
                sendNextPendingMessage();
}

void
TimelineView::sendNextPendingMessage()
{
        if (pending_msgs_.size() == 0)
                return;

        PendingMessage &m = pending_msgs_.head();
        switch (m.ty) {
        case matrix::events::MessageEventType::Audio:
        case matrix::events::MessageEventType::Image:
        case matrix::events::MessageEventType::File:
                // FIXME: Improve the API
                client_->sendRoomMessage(m.ty,
                                         m.txn_id,
                                         room_id_,
                                         QFileInfo(m.filename).fileName(),
                                         QFileInfo(m.filename),
                                         m.body);
                break;
        default:
                client_->sendRoomMessage(m.ty, m.txn_id, room_id_, m.body, QFileInfo());
                break;
        }
}

void
TimelineView::notifyForLastEvent()
{
        auto lastItem          = scroll_layout_->itemAt(scroll_layout_->count() - 1);
        auto *lastTimelineItem = qobject_cast<TimelineItem *>(lastItem->widget());

        if (lastTimelineItem)
                emit updateLastTimelineMessage(room_id_, lastTimelineItem->descriptionMessage());
        else
                qWarning() << "Cast to TimelineView failed" << room_id_;
}

bool
TimelineView::isPendingMessage(const QString &txnid,
                               const QString &sender,
                               const QString &local_userid)
{
        if (sender != local_userid)
                return false;

        for (const auto &msg : pending_msgs_) {
                if (QString::number(msg.txn_id) == txnid)
                        return true;
        }

        for (const auto &msg : pending_sent_msgs_) {
                if (QString::number(msg.txn_id) == txnid)
                        return true;
        }

        return false;
}

void
TimelineView::removePendingMessage(const QString &txnid)
{
        for (auto it = pending_sent_msgs_.begin(); it != pending_sent_msgs_.end(); ++it) {
                if (QString::number(it->txn_id) == txnid) {
                        int index = std::distance(pending_sent_msgs_.begin(), it);
                        pending_sent_msgs_.removeAt(index);
                        return;
                }
        }
        for (auto it = pending_msgs_.begin(); it != pending_msgs_.end(); ++it) {
                if (QString::number(it->txn_id) == txnid) {
                        int index = std::distance(pending_msgs_.begin(), it);
                        pending_msgs_.removeAt(index);
                        return;
                }
        }
}

void
TimelineView::handleFailedMessage(int txnid)
{
        Q_UNUSED(txnid);
        // Note: We do this even if the message has already been echoed.
        QTimer::singleShot(500, this, SLOT(sendNextPendingMessage()));
}

void
TimelineView::paintEvent(QPaintEvent *)
{
        QStyleOption opt;
        opt.init(this);
        QPainter p(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void
TimelineView::readLastEvent() const
{
        const auto eventId = getLastEventId();

        if (!eventId.isEmpty())
                client_->readEvent(room_id_, eventId);
}

QString
TimelineView::getLastEventId() const
{
        auto index = scroll_layout_->count();

        // Search backwards for the first event that has a valid event id.
        while (index > 0) {
                --index;

                auto lastItem          = scroll_layout_->itemAt(index);
                auto *lastTimelineItem = qobject_cast<TimelineItem *>(lastItem->widget());

                if (lastTimelineItem && !lastTimelineItem->eventId().isEmpty())
                        return lastTimelineItem->eventId();
        }

        return QString("");
}

void
TimelineView::showEvent(QShowEvent *event)
{
        readLastEvent();

        QWidget::showEvent(event);
}

bool
TimelineView::event(QEvent *event)
{
        if (event->type() == QEvent::WindowActivate) {
                QTimer::singleShot(1000, this, [=]() {
                        emit clearUnreadMessageCount(room_id_);
                        readLastEvent();
                });
        }

        return QWidget::event(event);
}
