/****************************************************************************
 **
 ** Copyright (C) 2014 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jolla.com>
 **
 ****************************************************************************/

#ifndef VKCALENDARSYNCADAPTOR_H
#define VKCALENDARSYNCADAPTOR_H

#include "vkdatatypesyncadaptor.h"

#include <QJsonObject>

#include <extendedcalendar.h>
#include <extendedstorage.h>
#include <notebook.h>

class VKCalendarSyncAdaptor : public VKDataTypeSyncAdaptor
{
    Q_OBJECT

public:
    VKCalendarSyncAdaptor(QObject *parent);
    ~VKCalendarSyncAdaptor();

    QString syncServiceName() const override;
    void sync(const QString &dataTypeString, int accountId = 0) override;

protected: // implementing VKDataTypeSyncAdaptor interface
    void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode) override;
    void beginSync(int accountId, const QString &accessToken) override;
    void finalize(int accountId) override;
    void finalCleanup() override;
    void retryThrottledRequest(const QString &request, const QVariantList &args, bool retryLimitReached) override;

private:
    void requestEvents(int accountId, const QString &accessToken, int offset = 0);

private Q_SLOTS:
    void finishedHandler();

private:
    QMap<int, QMap<QString, QJsonObject> > m_eventObjects;
    mKCal::ExtendedCalendar::Ptr m_calendar;
    mKCal::ExtendedStorage::Ptr m_storage;
    mKCal::Notebook::Ptr m_vkNotebook;
    bool m_storageNeedsSave;
};

#endif // VKCALENDARSYNCADAPTOR_H
