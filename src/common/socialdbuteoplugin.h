/****************************************************************************
 **
 ** Copyright (C) 2013-2014 Jolla Ltd.
 ** Contact: Raine Makelainen <raine.makelainen@jollamobile.com>
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

#ifndef SOCIALDBUTEOPLUGIN_H
#define SOCIALDBUTEOPLUGIN_H

#include <QtCore/qglobal.h>

// Buteo
#include <ClientPlugin.h>
#include <ProfileManager.h>
#include <PluginCbInterface.h>
#include <SyncCommonDefs.h>
#include <SyncProfile.h>
#include <SyncResults.h>

/*
   Datatype-specific implementations of this class
   allow per-account sync profiles for that data type.
*/

class SocialNetworkSyncAdaptor;
class Q_DECL_EXPORT SocialdButeoPlugin : public Buteo::ClientPlugin
{
    Q_OBJECT

protected:
    virtual SocialNetworkSyncAdaptor *createSocialNetworkSyncAdaptor() = 0;

public:
    SocialdButeoPlugin(const QString& pluginName,
                       const Buteo::SyncProfile& profile,
                       Buteo::PluginCbInterface *cbInterface,
                       const QString &socialServiceName,
                       const QString &dataTypeName);
    virtual ~SocialdButeoPlugin();

    bool init() override;
    bool uninit() override;
    bool startSync() override;
    void abortSync(Sync::SyncStatus status = Sync::SYNC_ABORTED) override;
    Buteo::SyncResults getSyncResults() const override;
    bool cleanUp() override;

public Q_SLOTS:
    void connectivityStateChanged(Sync::ConnectivityType type, bool state) override;

private Q_SLOTS:
    void syncStatusChanged();

protected:
    QList<Buteo::SyncProfile*> ensurePerAccountSyncProfilesExist();

private:
    void updateResults(const Buteo::SyncResults &results);
    Buteo::SyncResults m_syncResults;
    Buteo::ProfileManager m_profileManager;
    SocialNetworkSyncAdaptor *m_socialNetworkSyncAdaptor;
    QString m_socialServiceName;
    QString m_dataTypeName;
    int m_profileAccountId;
};

#endif // SOCIALDBUTEOPLUGIN_H
