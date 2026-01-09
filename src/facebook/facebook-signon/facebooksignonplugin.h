/****************************************************************************
 **
 ** Copyright (c) 2014 - 2021 Jolla Ltd.
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

#ifndef FACEBOOKSIGNONPLUGIN_H
#define FACEBOOKSIGNONPLUGIN_H

#include "socialdbuteoplugin.h"

#include <buteosyncfw5/SyncPluginLoader.h>

class Q_DECL_EXPORT FacebookSignonPlugin : public SocialdButeoPlugin
{
    Q_OBJECT

public:
    FacebookSignonPlugin(const QString& pluginName,
                  const Buteo::SyncProfile& profile,
                  Buteo::PluginCbInterface *cbInterface);
    ~FacebookSignonPlugin();

protected:
    SocialNetworkSyncAdaptor *createSocialNetworkSyncAdaptor() override;
};

class FacebookSignonPluginLoader : public Buteo::SyncPluginLoader
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.sailfishos.plugins.sync.FacebookSignonPluginLoader")
    Q_INTERFACES(Buteo::SyncPluginLoader)

public:
    Buteo::ClientPlugin* createClientPlugin(const QString& pluginName,
                                            const Buteo::SyncProfile& profile,
                                            Buteo::PluginCbInterface* cbInterface) override;
};

#endif // FACEBOOKSIGNONPLUGIN_H
