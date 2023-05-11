/****************************************************************************
 **
 ** Copyright (C) 2013-2014 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
 **
 ** This program/library is free software; you can redistribute it and/or
 ** modify it under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation.
 **
 ** This program/library is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 ** Lesser General Public License for more details.
 **
 ** You should have received a copy of the GNU Lesser General Public
 ** License along with this program/library; if not, write to the Free
 ** Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 ** 02110-1301 USA
 **
 ****************************************************************************/

#ifndef GOOGLECALENDARSYNCADAPTOR_H
#define GOOGLECALENDARSYNCADAPTOR_H

#include "googledatatypesyncadaptor.h"

#include <QtCore/QString>
#include <QtCore/QMultiMap>
#include <QtCore/QPair>
#include <QtCore/QJsonObject>

#include <extendedcalendar.h>
#include <extendedstorage.h>
#include <KCalendarCore/ICalFormat>

class GoogleCalendarSyncAdaptor : public GoogleDataTypeSyncAdaptor
{
    Q_OBJECT

public:
    GoogleCalendarSyncAdaptor(QObject *parent);
    ~GoogleCalendarSyncAdaptor();

    QString syncServiceName() const;
    void sync(const QString &dataTypeString, int accountId);

protected: // implementing GoogleDataTypeSyncAdaptor interface
    void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode);
    void beginSync(int accountId, const QString &accessToken);
    void finalCleanup();

private:
    enum ChangeType {
        NoChange = 0,
        Insert = 1,
        Modify = 2,
        Delete = 3,
        DeleteOccurrence = 4, // used to identify downsynced status->CANCELLED changes only
        CleanSync = 5 // delete followed by insert.
    };

    enum AccessRole {
        NoAccess = 0,
        FreeBusyReader = 1,
        Reader = 2,
        Writer = 3,
        Owner = 4
    };

    enum SyncFailure {
        NoSyncFailure,
        UploadFailure,
        UpdateFailure,
        DeleteFailure
    };

    struct UpsyncChange {
        UpsyncChange() : upsyncType(NoChange) {}
        QString accessToken;
        ChangeType upsyncType;
        QString kcalNotebookId;
        QString kcalEventId;
        QDateTime recurrenceId;
        QString calendarId;
        QString eventId;
        QByteArray eventData;
    };

    struct CalendarInfo {
        CalendarInfo() : change(NoChange), access(NoAccess) {}
        QString summary;
        QString description;
        QString color;
        ChangeType change;
        AccessRole access;
    };

    void requestCalendars(const QString &accessToken,
                          bool needCleanSync, const QString &pageToken = QString());
    void requestEvents(const QString &accessToken,
                       const QString &calendarId, const QString &syncToken,
                       const QString &pageToken = QString());
    void updateLocalCalendarNotebooks(const QString &accessToken, bool needCleanSync);
    QList<UpsyncChange> determineSyncDelta(const QString &accessToken,
                                           const QString &calendarId, const QDateTime &since);

    void queueSequencedInsertion(QList<UpsyncChange> &changesToUpsync,
                                 const KCalendarCore::Event::Ptr event,
                                 const QString &calendarId,
                                 const QString &accessToken);

    void reInsertWithRandomId(const QNetworkReply *reply);
    void upsyncChanges(const UpsyncChange &changeToUpsync);

    void applyRemoteChangesLocally();
    void updateLocalCalendarNotebookEvents(const QString &calendarId);

    mKCal::Notebook::Ptr notebookForCalendarId(const QString &calendarId) const;
    void finishedRequestingRemoteEvents(const QString &accessToken,
                                        const QString &calendarId, const QString &syncToken,
                                        const QString &nextSyncToken, const QDateTime &since);
    void clampEventTimeToSync(KCalendarCore::Event::Ptr event) const;
    bool isCleanSync(const QString &calendarId) const;

    static void setCalendarProperties(mKCal::Notebook::Ptr notebook,
                                      const CalendarInfo &calendarInfo,
                                      const QString &serverCalendarId,
                                      int accountId,
                                      const QString &syncProfile,
                                      const QString &ownerEmail);

    const QList<QDateTime> getExceptionInstanceDates(const KCalendarCore::Event::Ptr event) const;
    QJsonObject kCalToJson(KCalendarCore::Event::Ptr event, KCalendarCore::ICalFormat &icalFormat, bool setUidProperty = false) const;

    void handleErrorReply(QNetworkReply *reply);
    void handleDeleteReply(QNetworkReply *reply);
    void handleInsertModifyReply(QNetworkReply *reply);
    void performSequencedUpsyncs(const QNetworkReply *reply);

    KCalendarCore::Event::Ptr addDummyParent(const QJsonObject &eventData,
                                             const QString &parentId,
                                             const mKCal::Notebook::Ptr googleNotebook);

    bool applyRemoteDelete(const QString &eventId,
                           QMap<QString, KCalendarCore::Event::Ptr> &allLocalEventsMap);
    bool applyRemoteDeleteOccurence(const QString &eventId,
                                    const QJsonObject &eventData,
                                    QMap<QString, KCalendarCore::Event::Ptr> &allLocalEventsMap);
    bool applyRemoteModify(const QString &eventId,
                           const QJsonObject &eventData,
                           const QString &calendarId,
                           QMap<QString, KCalendarCore::Event::Ptr> &allLocalEventsMap);
    bool applyRemoteInsert(const QString &eventId,
                           const QJsonObject &eventData,
                           const QString &calendarId,
                           const QHash<QString, QString> &upsyncedUidMapping,
                           QMap<QString, KCalendarCore::Event::Ptr> &allLocalEventsMap);


    void flagUploadFailure(const QString &kcalEventId);
    void flagUploadSuccess(const QString &kcalEventId);
    void flagUpdateSuccess(const QString &kcalEventId);
    void flagDeleteFailure(const QString &kcalEventId);
    void applySyncFailureFlag(KCalendarCore::Event::Ptr event, SyncFailure flag);
    void applySyncFailureFlags();

private Q_SLOTS:
    void calendarsFinishedHandler();
    void eventsFinishedHandler();
    void upsyncFinishedHandler();

private:
    QMap<QString, CalendarInfo> m_serverCalendarIdToCalendarInfo;
    QMap<QString, int> m_serverCalendarIdToDefaultReminderTimes;
    QMultiMap<QString, QJsonObject> m_calendarIdToEventObjects;
    QMap<QString, QString> m_recurringEventIdToKCalUid;
    bool m_syncSucceeded;
    int m_accountId;

    QStringList m_calendarsBeingRequested;               // calendarIds
    QStringList m_calendarsFinishedRequested;            // calendarId to updated timestamp string
    QMap<QString, QString> m_calendarsThisSyncTokens;    // calendarId to sync token used during this sync cycle
    QMap<QString, QString> m_calendarsNextSyncTokens;    // calendarId to sync token to use during next sync cycle
    QMap<QString, QDateTime> m_calendarsSyncDate;        // calendarId to since date to use when determining delta
    QMultiMap<QString, QPair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject> > m_changesFromDownsync; // calendarId to change
    QMultiMap<QString, QPair<KCalendarCore::Event::Ptr, QJsonObject> > m_changesFromUpsync; // calendarId to event+upsyncResponse
    QSet<QString> m_syncTokenFailure; // calendarIds suffering from 410 error due to invalid sync token
    QSet<QString> m_timeMinFailure;   // calendarIds suffering from 410 error due to invalid timeMin value
    QMap<QString, KCalendarCore::Incidence::List> m_purgeList; // notebookIds to local deleted incidences that can be purged
    QMap<QString, KCalendarCore::Incidence::Ptr> m_deletedGcalIdToIncidence;

    mKCal::ExtendedCalendar::Ptr m_calendar;
    mKCal::ExtendedStorage::Ptr m_storage;
    mutable KCalendarCore::ICalFormat m_icalFormat;
    bool m_storageNeedsSave;
    QDateTime m_syncedDateTime;
    // Sequenced upsync changes are referenced by the gcalId of the
    // parent upsync, as recorded in UpsyncChange::eventId
    QMultiHash<QString, UpsyncChange> m_sequenced;
    int m_collisionErrorCount;
    QMap<QString, SyncFailure> m_eventSyncFlags;
};

#endif // GOOGLECALENDARSYNCADAPTOR_H
