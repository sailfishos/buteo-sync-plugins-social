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

#include <buteosyncfw5/LogMacros.h>
#include <buteosyncfw5/PluginCbInterface.h>
#include <buteosyncfw5/ProfileEngineDefs.h>
#include <buteosyncfw5/ProfileManager.h>
#include "knowncontactsplugin.h"
#include "knowncontactssyncer.h"

const auto KnownContactsSyncFolder = QStringLiteral("system/privileged/Contacts/knowncontacts");

extern "C" KnownContactsPlugin* createPlugin(const QString& pluginName,
                                             const Buteo::SyncProfile& profile,
                                             Buteo::PluginCbInterface *cbInterface)
{
    return new KnownContactsPlugin(pluginName, profile, cbInterface);
}

extern "C" void destroyPlugin(KnownContactsPlugin *client)
{
    delete client;
}

KnownContactsPlugin::KnownContactsPlugin(const QString& pluginName,
                                         const Buteo::SyncProfile& profile,
                                         Buteo::PluginCbInterface *cbInterface)
    : Buteo::ClientPlugin(pluginName, profile, cbInterface)
    , m_syncer(nullptr)
{
    FUNCTION_CALL_TRACE;
}

KnownContactsPlugin::~KnownContactsPlugin()
{
    FUNCTION_CALL_TRACE;
}

/**
  * \!brief Initialize the plugin for the actual sync to happen
  * This method will be invoked by the framework
  */
bool KnownContactsPlugin::init()
{
    FUNCTION_CALL_TRACE;

    if (!m_syncer) {
        auto path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                               QDir::separator() + KnownContactsSyncFolder;
        m_syncer = new KnownContactsSyncer(path, this);
        LOG_DEBUG("KnownContacts plugin initialized for path" << path);
    }

    return true;
}

/**
  * \!brief Uninitializes the plugin
  * This method will be invoked by the framework
  */
bool KnownContactsPlugin::uninit()
{
    FUNCTION_CALL_TRACE;

    delete m_syncer;
    m_syncer = nullptr;

    LOG_DEBUG("KnownContacts plugin uninitialized");
    return true;
}

/**
  * \!brief Start the actual sync. This method will be invoked by the
  * framework
  */
bool KnownContactsPlugin::startSync()
{
    FUNCTION_CALL_TRACE;

    if (!m_syncer)
        return false;

    connect(m_syncer, &KnownContactsSyncer::syncSucceeded,
            this, &KnownContactsPlugin::syncSucceeded);

    connect(m_syncer, &KnownContactsSyncer::syncFailed,
            this, &KnownContactsPlugin::syncFailed);

    LOG_DEBUG("Starting sync");

    // Start the actual sync
    return m_syncer->startSync();
}

/**
  * \!brief Aborts sync. An abort can happen due to protocol errors,
  * connection failures or by the user (via a UI)
  */
void KnownContactsPlugin::abortSync(Sync::SyncStatus status)
{
    Q_UNUSED(status)
    FUNCTION_CALL_TRACE;

    LOG_DEBUG("Aborting is not supported");
    // Not supported, syncing usually takes very little time
    // and there is not much to abort
}

Buteo::SyncResults KnownContactsPlugin::getSyncResults() const
{
    FUNCTION_CALL_TRACE;

    return m_results;
}

/**
  * This method is required if a profile has been deleted. The plugin
  * has to implement the necessary cleanup (like temporary data, anchors etc.)
  */
bool KnownContactsPlugin::cleanUp()
{
    FUNCTION_CALL_TRACE;

    bool success;

    init();  // Ensure that syncer exists
    success = m_syncer->purgeData();
    uninit();  // Destroy syncer

    return success;
}

void KnownContactsPlugin::syncSucceeded()
{
    FUNCTION_CALL_TRACE;

    LOG_DEBUG("Sync successful");
    m_results = Buteo::SyncResults(QDateTime::currentDateTimeUtc(),
                                   Buteo::SyncResults::SYNC_RESULT_SUCCESS,
                                   Buteo::SyncResults::NO_ERROR);
    emit success(getProfileName(), QStringLiteral("Success"));
}

void KnownContactsPlugin::syncFailed()
{
    FUNCTION_CALL_TRACE;

    LOG_DEBUG("Sync failed");
    m_results = Buteo::SyncResults(iProfile.lastSuccessfulSyncTime(),
                                   Buteo::SyncResults::SYNC_RESULT_FAILED,
                                   Buteo::SyncResults::INTERNAL_ERROR);
    emit error(getProfileName(), QStringLiteral("Failure"), Buteo::SyncResults::INTERNAL_ERROR);
}

/**
  * Signal from the protocol engine about connectivity state changes
  */
void KnownContactsPlugin::connectivityStateChanged(Sync::ConnectivityType type, bool state)
{
    Q_UNUSED(type)
    Q_UNUSED(state)
    FUNCTION_CALL_TRACE;
    // Stub
}
