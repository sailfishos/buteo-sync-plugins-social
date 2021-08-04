/****************************************************************************
 **
 ** Copyright (C) 2013 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
 **
 ****************************************************************************/

#include "vknotificationsyncadaptor.h"
#include "trace.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QUrlQuery>
#include <QDebug>

//static const int OLD_NOTIFICATION_LIMIT_IN_DAYS = 21; // TODO

VKNotificationSyncAdaptor::VKNotificationSyncAdaptor(QObject *parent)
    : VKDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Notifications, parent)
{
    setInitialActive(true);
}

VKNotificationSyncAdaptor::~VKNotificationSyncAdaptor()
{
}

QString VKNotificationSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("vk-microblog");
}

void VKNotificationSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    m_db.removeNotifications(oldId);
    m_db.sync();
    m_db.wait();
}

void VKNotificationSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    requestNotifications(accountId, accessToken);
}

void VKNotificationSyncAdaptor::finalize(int accountId)
{
    Q_UNUSED(accountId);
    if (syncAborted()) {
        qCDebug(lcSocialPlugin) << "sync aborted, skipping finalize of VK Notifications from account:" << accountId;
    } else {
        qCDebug(lcSocialPlugin) << "finalizing VK Notifications sync with account:" << accountId;
        //m_db.purgeOldNotifications(OLD_NOTIFICATION_LIMIT_IN_DAYS); // TODO
        Q_FOREACH (const NotificationData &notification, m_notificationsToAdd) {
            QList<UserProfile> userProfiles;
            foreach (const QJsonValue &entry, notification.profiles) {
                userProfiles << UserProfile::fromJsonObject(entry.toObject());
            }
            saveVKNotificationFromObject(notification.accountId, notification.notification, userProfiles);
        }
        m_db.sync();
        m_db.wait();
    }
}

void VKNotificationSyncAdaptor::retryThrottledRequest(const QString &request, const QVariantList &args, bool retryLimitReached)
{
    int accountId = args[0].toInt();
    if (retryLimitReached) {
        qCWarning(lcSocialPlugin) << "hit request retry limit! unable to request data from VK account with id" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
    } else {
        qCDebug(lcSocialPlugin) << "retrying Notifications" << request << "request for VK account:" << accountId;
        requestNotifications(accountId, args[1].toString(), args[2].toString(), args[3].toString());
    }
    decrementSemaphore(accountId); // finished waiting for the request.
}

void VKNotificationSyncAdaptor::requestNotifications(int accountId, const QString &accessToken, const QString &until, const QString &pagingToken)
{
    // TODO: result paging
    Q_UNUSED(until);
    Q_UNUSED(pagingToken);

    QList<QPair<QString, QString> > queryItems;
    queryItems.append(QPair<QString, QString>(QString(QLatin1String("access_token")), accessToken));
    queryItems.append(QPair<QString, QString>(QString(QLatin1String("v")), QStringLiteral("5.21"))); // version

    QUrl url(QStringLiteral("https://api.vk.com/method/notifications.get"));
    QUrlQuery query(url);
    query.setQueryItems(queryItems);
    url.setQuery(query);
    QNetworkReply *reply = m_networkAccessManager->get(QNetworkRequest(url));

    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(finishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        // request was throttled by VKNetworkAccessManager
        QVariantList args;
        args << accountId << accessToken << until << pagingToken;
        enqueueThrottledRequest(QStringLiteral("requestNotifications"), args);

        // we are waiting to request data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId); // decremented in retryThrottledRequest().
    }
}

void VKNotificationSyncAdaptor::finishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();
    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);

    if (!isError && ok && parsed.contains(QLatin1String("response"))) {
        QJsonObject responseObj = parsed.value(QStringLiteral("response")).toObject();

        QJsonArray profileValues = responseObj.value(QStringLiteral("profiles")).toArray();
        QJsonArray items = responseObj.value(QLatin1String("items")).toArray();
        foreach (const QJsonValue &entry, items) {
            QJsonObject object = entry.toObject();
            if (!object.isEmpty()) {
                m_notificationsToAdd.append(NotificationData(accountId, object, profileValues));
            } else {
                qCDebug(lcSocialPlugin) << "notification object empty; skipping";
            }
        }
    } else {
        // error occurred during request.
        qCWarning(lcSocialPlugin) << "error: unable to parse notification data from request with account:" << accountId <<
                          "got:" << QString::fromUtf8(replyData);
    }

    // we're finished this request.  Decrement our busy semaphore.
    decrementSemaphore(accountId);
}

void VKNotificationSyncAdaptor::saveVKNotificationFromObject(int accountId, const QJsonObject &notif, const QList<UserProfile> &userProfiles)
{
    QString type = notif.value("type").toString();
    QDateTime timestamp = parseVKDateTime(notif.value(QStringLiteral("date")));
    QJsonObject feedback = notif.value(QStringLiteral("feedback")).toObject();
    QJsonArray feedbackItems = feedback.value(QStringLiteral("items")).toArray();
    Q_FOREACH (const QJsonValue &feedbackItem, feedbackItems) {
        QJsonObject feedbackItemObj = feedbackItem.toObject();
        int fromId = int(feedbackItemObj.value(QStringLiteral("from_id")).toDouble());
        int toId = int(feedbackItemObj.value(QStringLiteral("to_id")).toDouble());
        UserProfile profile = findUserProfile(userProfiles, fromId);
        if (profile.uid != 0) {
            qCDebug(lcSocialPluginTrace) << "adding VK notification:" << type;
            m_db.addVKNotification(accountId, type, QString::number(fromId), profile.name(), profile.icon, QString::number(toId), timestamp);
        } else {
            qCDebug(lcSocialPlugin) << "no user profile found for owner" << fromId << "of notification from account" << accountId;
        }
    }
}
