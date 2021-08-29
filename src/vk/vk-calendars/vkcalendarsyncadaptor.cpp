/****************************************************************************
 **
 ** Copyright (C) 2014 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jolla.com>
 **
 ****************************************************************************/

#include "vkcalendarsyncadaptor.h"
#include "trace.h"

#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

#include <extendedcalendar.h>
#include <extendedstorage.h>

#define SOCIALD_VK_NAME "VK"
#define SOCIALD_VK_COLOR "#45668e"
#define SOCIALD_VK_MAX_CALENDAR_ENTRY_RESULTS 100

namespace {
    bool eventNeedsUpdate(KCalendarCore::Event::Ptr event, const QJsonObject &json)
    {
        // TODO: compare data, determine if we need to update
        Q_UNUSED(event)
        Q_UNUSED(json)
        return true;
    }
    void jsonToKCal(const QString &vkId, const QJsonObject &json, KCalendarCore::Event::Ptr event, bool isUpdate)
    {
        qCDebug(lcSocialPlugin) << "Converting group event JSON to calendar event:" << json;
        if (!isUpdate) {
            QString eventUid = QUuid::createUuid().toString().mid(1);
            eventUid.chop(1);
            eventUid += QStringLiteral(":%1").arg(vkId);
            event->setUid(eventUid);
        }
        event->setSummary(json.value(QStringLiteral("name")).toString());
        event->setDescription(json.value(QStringLiteral("description")).toString());
        QString eventAddress = json.value(QStringLiteral("place")).toObject().value(QStringLiteral("address")).toString();
        QString addressTitle = json.value(QStringLiteral("place")).toObject().value(QStringLiteral("title")).toString();
        event->setLocation(eventAddress.isEmpty() ? addressTitle : eventAddress);
        if (json.contains(QStringLiteral("start_date"))) {
            uint startTime = json.value(QStringLiteral("start_date")).toDouble();
            event->setDtStart(QDateTime::fromTime_t(startTime));
            if (json.contains(QStringLiteral("end_date"))) {
                uint endTime = json.value(QStringLiteral("end_date")).toDouble();
                event->setDtEnd(QDateTime::fromTime_t(endTime));
            }
        }
    }
}

VKCalendarSyncAdaptor::VKCalendarSyncAdaptor(QObject *parent)
    : VKDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Calendars, parent)
    , m_calendar(mKCal::ExtendedCalendar::Ptr(new mKCal::ExtendedCalendar(QTimeZone::utc())))
    , m_storage(mKCal::ExtendedCalendar::defaultStorage(m_calendar))
    , m_storageNeedsSave(false)
{
    setInitialActive(true);
}

VKCalendarSyncAdaptor::~VKCalendarSyncAdaptor()
{
}

QString VKCalendarSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("vk-calendars");
}

void VKCalendarSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    m_storageNeedsSave = false;
    m_storage->open(); // we close it in finalCleanup()
    VKDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void VKCalendarSyncAdaptor::finalize(int accountId)
{
    if (syncAborted()) {
        qCDebug(lcSocialPlugin) << "sync aborted, skipping finalize of VK calendar events from account:" << accountId;
    } else {
        qCDebug(lcSocialPlugin) << "finalizing VK calendar sync with account:" << accountId;
        // convert the m_eventObjects to mkcal events, store in db or remove as required.
        bool foundVkNotebook = false;
        Q_FOREACH (mKCal::Notebook::Ptr notebook, m_storage->notebooks()) {
            if (notebook->pluginName() == QStringLiteral(SOCIALD_VK_NAME)
                    && notebook->account() == QString::number(accountId)) {
                foundVkNotebook = true;
                m_vkNotebook = notebook;
            }
        }

        if (!foundVkNotebook) {
            m_vkNotebook = mKCal::Notebook::Ptr(new mKCal::Notebook);
            m_vkNotebook->setUid(QUuid::createUuid().toString());
            m_vkNotebook->setName(QStringLiteral("VKontakte"));
            m_vkNotebook->setColor(QStringLiteral(SOCIALD_VK_COLOR));
            m_vkNotebook->setPluginName(QStringLiteral(SOCIALD_VK_NAME));
            m_vkNotebook->setAccount(QString::number(accountId));
            m_vkNotebook->setIsReadOnly(false); // temporarily
            m_storage->addNotebook(m_vkNotebook);
            m_storageNeedsSave = true;
        }

        // We've found the notebook for this account.
        // Build up a map of existing events, then determine A/M/R delta.
        int addedCount = 0, modifiedCount = 0, removedCount = 0;
        m_storage->loadNotebookIncidences(m_vkNotebook->uid());
        KCalendarCore::Incidence::List allIncidences;
        m_storage->allIncidences(&allIncidences, m_vkNotebook->uid());
        QSet<QString> serverSideEventIds = m_eventObjects[accountId].keys().toSet();
        Q_FOREACH (const KCalendarCore::Incidence::Ptr incidence, allIncidences) {
            KCalendarCore::Event::Ptr event = m_calendar->event(incidence->uid());
            // when we add new events, we generate the uid like QUUID:vkId
            // to ensure that even after removal/re-add, the uid is unique.
            const QString &eventUid = event->uid();
            int vkIdIdx = eventUid.indexOf(':') + 1;
            QString vkId = (vkIdIdx > 0 && eventUid.size() > vkIdIdx) ? eventUid.mid(eventUid.indexOf(':') + 1) : QString();
            if (!m_eventObjects[accountId].contains(vkId)) {
                // this event was removed server-side since last sync.
                m_vkNotebook->setIsReadOnly(false); // temporarily
                m_storageNeedsSave = true;
                m_calendar->deleteIncidence(event);
                removedCount += 1;
                qCDebug(lcSocialPluginTrace) << "deleted existing event:" << event->summary() << ":" << event->dtStart().toString();
            } else {
                // this event was possibly modified server-side.
                const QJsonObject &eventObject(m_eventObjects[accountId][vkId]);
                if (eventNeedsUpdate(event, eventObject)) {
                    m_vkNotebook->setIsReadOnly(false); // temporarily
                    event->startUpdates();
                    event->setReadOnly(false);
                    jsonToKCal(vkId, eventObject, event, true);
                    event->setReadOnly(true);
                    event->endUpdates();
                    m_storageNeedsSave = true;
                    modifiedCount += 1;
                    qCDebug(lcSocialPluginTrace) << "modified existing event:" << event->summary() << ":" << event->dtStart().toString();
                } else {
                    qCDebug(lcSocialPluginTrace) << "no modificiation necessary for existing event:" << event->summary() << ":" << event->dtStart().toString();
                }
                serverSideEventIds.remove(vkId);
            }
        }

        // if we have any left over, they're additions.
        Q_FOREACH (const QString &vkId, serverSideEventIds) {
            m_vkNotebook->setIsReadOnly(false); // temporarily
            const QJsonObject &eventObject(m_eventObjects[accountId][vkId]);
            KCalendarCore::Event::Ptr event = KCalendarCore::Event::Ptr(new KCalendarCore::Event);
            jsonToKCal(vkId, eventObject, event, false); // direct conversion
            event->setReadOnly(true);
            if (!m_calendar->addEvent(event, m_vkNotebook->uid())) {
                qCDebug(lcSocialPluginTrace) << "failed to add new event:" << event->summary() << ":" << event->dtStart().toString() << "to notebook:" << m_vkNotebook->uid();
                continue;
            }
            m_storageNeedsSave = true;
            addedCount += 1;
            qCDebug(lcSocialPluginTrace) << "added new event:" << event->summary() << ":" << event->dtStart().toString() << "to notebook:" << m_vkNotebook->uid();
        }

        // finished!
        qCInfo(lcSocialPlugin) << "finished calendars sync with VK account" << accountId <<
                         ": got A/M/R:" << addedCount << "/" << modifiedCount << "/" << removedCount;
    }
}

void VKCalendarSyncAdaptor::finalCleanup()
{
    // commit changes to db
    if (m_storageNeedsSave && !syncAborted()) {
        qCDebug(lcSocialPlugin) << "saving changes in VK calendar to storage";
        m_storage->save();
        if (m_vkNotebook) {
            // the notebook will have been set writable.  make the notebook read-only again.
            m_vkNotebook->setIsReadOnly(true);
            m_storage->updateNotebook(m_vkNotebook);
            m_storage->save();
        }
    } else {
        qCDebug(lcSocialPlugin) << "no changes to VK calendar - not saving storage";
    }
    m_calendar->close();
    m_storage->close();
}

void VKCalendarSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode)
{
    // Delete the notebook and all events in it from the storage
    qCDebug(lcSocialPlugin) << "Purging calendar data for account:" << oldId;
    if (mode == SocialNetworkSyncAdaptor::CleanUpPurge) {
        // need to initialise the database
        m_storage->open();
    }
    Q_FOREACH (mKCal::Notebook::Ptr notebook, m_storage->notebooks()) {
        if (notebook->pluginName() == QStringLiteral(SOCIALD_VK_NAME)
                && notebook->account() == QString::number(oldId)) {
            qCDebug(lcSocialPlugin) << "Purging notebook:" << notebook->uid() << "associated with account:" << oldId;
            notebook->setIsReadOnly(false);
            m_storage->deleteNotebook(notebook);
            m_storageNeedsSave = true;
        }
    }
    if (mode == SocialNetworkSyncAdaptor::CleanUpPurge) {
        // and commit any changes made.
        finalCleanup();
    }
}

void VKCalendarSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    qCDebug(lcSocialPlugin) << "Beginning Calendar sync for VK, account:" << accountId;
    m_eventObjects[accountId].clear();
    requestEvents(accountId, accessToken);
}

void VKCalendarSyncAdaptor::retryThrottledRequest(const QString &request, const QVariantList &args, bool retryLimitReached)
{
    int accountId = args[0].toInt();
    if (retryLimitReached) {
        qCWarning(lcSocialPlugin) << "hit request retry limit! unable to request data from VK account with id" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
    } else {
        qCDebug(lcSocialPlugin) << "retrying Calendars" << request << "request for VK account:" << accountId;
        requestEvents(accountId, args[1].toString(), args[2].toInt());
    }
    decrementSemaphore(accountId); // finished waiting for the request.
}

void VKCalendarSyncAdaptor::requestEvents(int accountId, const QString &accessToken, int offset)
{
    QUrlQuery urlQuery;
    QUrl requestUrl = QUrl(QStringLiteral("https://api.vk.com/method/groups.get"));
    urlQuery.addQueryItem("v", QStringLiteral("5.21")); // version
    urlQuery.addQueryItem("access_token", accessToken);
    if (offset >= 1) urlQuery.addQueryItem ("offset", QString::number(offset));
    urlQuery.addQueryItem("count", QString::number(SOCIALD_VK_MAX_CALENDAR_ENTRY_RESULTS));
    urlQuery.addQueryItem("extended", QStringLiteral("1"));
    urlQuery.addQueryItem("fields", QStringLiteral("start_date,end_date,place,description"));
    // theoretically, could use filter=events but this always returns zero results.

    requestUrl.setQuery(urlQuery);
    QNetworkReply *reply = m_networkAccessManager->get(QNetworkRequest(requestUrl));

    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("offset", offset);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
                this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)),
                this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(finishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        // request was throttled by VKNetworkAccessManager
        QVariantList args;
        args << accountId << accessToken << offset;
        enqueueThrottledRequest(QStringLiteral("requestEvents"), args);

        // we are waiting to request data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId); // decremented in retryThrottledRequest().
    }
}

void VKCalendarSyncAdaptor::finishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    int offset = reply->property("offset").toInt();
    QByteArray replyData = reply->readAll();
    bool isError = reply->property("isError").toBool();

    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (!isError && ok) {
        // the zeroth index contains the count of response items
        QJsonArray items = parsed.value("response").toObject().value("items").toArray();
        int count = parsed.value("response").toObject().value("count").toInt();
        qCDebug(lcSocialPlugin) << "total communities returned in request with account" << accountId << ":" << count;
        bool needMorePages = false;
        if (count == 0 || count < SOCIALD_VK_MAX_CALENDAR_ENTRY_RESULTS) {
            // finished retrieving events.
        } else {
            needMorePages = true;
        }

        // parse the data in this page of results.
        for (int i = 1; i < items.size(); ++i) {
            QJsonObject currEvent = items.at(i).toObject();
            if (currEvent.isEmpty() || currEvent.value("type").toString() != QStringLiteral("event")) {
                qCDebug(lcSocialPlugin) << "ignoring community:" << currEvent.value("name").toString() << "as it is not an event";
                continue;
            }

            int gid = 0;
            if (currEvent.value("id").toDouble() > 0) {
                gid = currEvent.value("id").toInt();
            } else if (currEvent.value("gid").toInt() > 0) {
                gid = currEvent.value("gid").toInt();
            }
            if (gid > 0) {
                // we just cache them in memory here; we store them only if all
                // events are retrieved without sync being aborted / connection loss.
                QString gidstr = QString::number(gid);
                m_eventObjects[accountId].insert(gidstr, currEvent);
                qCDebug(lcSocialPlugin) << "Have found event with id:" << gid << ":" << currEvent.value("name").toString();
            } else {
                qWarning() << "event has no id:" << currEvent;
            }
        }

        // if we need to request more data, do so.  otherwise, parse all of the results into mkcal events.
        if (needMorePages) {
            qCDebug(lcSocialPlugin) << "need to fetch more pages of calendar results";
            requestEvents(accountId, accessToken, offset + SOCIALD_VK_MAX_CALENDAR_ENTRY_RESULTS);
        } else {
            qCDebug(lcSocialPlugin) << "done fetching calendar results";
        }
    } else {
        QVariantList args;
        args << accountId << accessToken << offset;
        if (enqueueServerThrottledRequestIfRequired(parsed, QStringLiteral("requestEvents"), args)) {
            // we hit the throttle limit, let throttle timer repeat the request
            // don't decrement semaphore yet as we're still waiting for it.
            // it will be decremented in retryThrottledRequest().
            return;
        }

        // error occurred during request.
        qCWarning(lcSocialPlugin) << "unable to parse calendar data from request with account" << accountId <<
                          "; got:" << QString::fromUtf8(replyData);
    }

    // we're finished this request.  Decrement our busy semaphore.
    decrementSemaphore(accountId);
}
