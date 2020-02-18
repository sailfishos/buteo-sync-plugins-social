/****************************************************************************
 **
 ** Copyright (c) 2020 Open Mobile Platform LLC.
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

#include "dropboxbackupqueryplugin.h"
#include "dropboxbackupquerysyncadaptor.h"
#include "socialnetworksyncadaptor.h"

extern "C" DropboxBackupQueryPlugin* createPlugin(const QString& pluginName,
                                       const Buteo::SyncProfile& profile,
                                       Buteo::PluginCbInterface *callbackInterface)
{
    return new DropboxBackupQueryPlugin(pluginName, profile, callbackInterface);
}

extern "C" void destroyPlugin(DropboxBackupQueryPlugin* plugin)
{
    delete plugin;
}

DropboxBackupQueryPlugin::DropboxBackupQueryPlugin(const QString& pluginName,
                             const Buteo::SyncProfile& profile,
                             Buteo::PluginCbInterface *callbackInterface)
    : SocialdButeoPlugin(pluginName, profile, callbackInterface,
                         QStringLiteral("dropbox"),
                         SocialNetworkSyncAdaptor::dataTypeName(SocialNetworkSyncAdaptor::BackupQuery))
{
}

DropboxBackupQueryPlugin::~DropboxBackupQueryPlugin()
{
}

SocialNetworkSyncAdaptor *DropboxBackupQueryPlugin::createSocialNetworkSyncAdaptor()
{
    return new DropboxBackupQuerySyncAdaptor(this);
}
