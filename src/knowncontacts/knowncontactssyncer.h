/*
 * Buteo sync plugin that stores locally created contacts
 * Copyright (C) 2020 Open Mobile Platform LLC.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * and appearing in the file LICENSE.LGPL included in the packaging
 * of this file.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301  USA
 */

#ifndef KNOWNCONTACTS_SYNCER_H
#define KNOWNCONTACTS_SYNCER_H

#include <QContact>
#include <QContactCollection>

#include <QHash>
#include <QSettings>

#include <twowaycontactsyncadaptor.h>

QTCONTACTS_USE_NAMESPACE

class KnownContactsSyncer : public QObject, public QtContactsSqliteExtensions::TwoWayContactSyncAdaptor
{
    Q_OBJECT

public:
    KnownContactsSyncer(QString path, QObject *parent = nullptr);
    ~KnownContactsSyncer();

    bool purgeData();

    virtual bool determineRemoteCollections() override;
    virtual bool deleteRemoteCollection(const QContactCollection &collection) override;
    virtual bool determineRemoteContacts(const QContactCollection &collection) override;
    virtual bool storeLocalChangesRemotely(const QContactCollection &collection,
                                           const QList<QContact> &addedContacts,
                                           const QList<QContact> &modifiedContacts,
                                           const QList<QContact> &deletedContacts) override;
    virtual void storeRemoteChangesLocally(const QContactCollection &collection,
                                           const QList<QContact> &addedContacts,
                                           const QList<QContact> &modifiedContacts,
                                           const QList<QContact> &deletedContacts) override;
    virtual void syncFinishedSuccessfully() override;
    virtual void syncFinishedWithError() override;

signals:
    void syncSucceeded();
    void syncFailed();

private:
    void readContacts(QSettings *file, QHash<QString, QContact> *contacts);

    QContactCollection m_collection;
    QString m_syncFolder;
};

#endif // KNOWNCONTACTS_SYNCER_H
