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

#ifndef DROPBOXBACKUPQUERYPLUGIN_H
#define DROPBOXBACKUPQUERYPLUGIN_H

#include "socialdbuteoplugin.h"

class SOCIALDBUTEOPLUGIN_EXPORT DropboxBackupQueryPlugin : public SocialdButeoPlugin
{
    Q_OBJECT

public:
    DropboxBackupQueryPlugin(const QString& pluginName,
                  const Buteo::SyncProfile& profile,
                  Buteo::PluginCbInterface *cbInterface);
    ~DropboxBackupQueryPlugin();

protected:
    SocialNetworkSyncAdaptor *createSocialNetworkSyncAdaptor();
};

extern "C" DropboxBackupQueryPlugin* createPlugin(const QString& pluginName,
                                              const Buteo::SyncProfile& profile,
                                              Buteo::PluginCbInterface *cbInterface);

extern "C" void destroyPlugin(DropboxBackupQueryPlugin* client);

#endif // DROPBOXBACKUPQUERYPLUGIN_H
