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

#ifndef GOOGLECALENDARSPLUGIN_H
#define GOOGLECALENDARSPLUGIN_H

#include "socialdbuteoplugin.h"
#include <buteosyncfw5/SyncPluginLoader.h>

class Q_DECL_EXPORT GoogleCalendarsPlugin : public SocialdButeoPlugin
{
    Q_OBJECT

public:
    GoogleCalendarsPlugin(const QString& pluginName,
                  const Buteo::SyncProfile& profile,
                  Buteo::PluginCbInterface *cbInterface);
    ~GoogleCalendarsPlugin();

protected:
    SocialNetworkSyncAdaptor *createSocialNetworkSyncAdaptor() override;
};

class GoogleCalendarPluginLoader : public Buteo::SyncPluginLoader
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.sailfishos.plugins.sync.GoogleCalendarPluginLoader")
    Q_INTERFACES(Buteo::SyncPluginLoader)

public:
    Buteo::ClientPlugin* createClientPlugin(const QString& pluginName,
                                            const Buteo::SyncProfile& profile,
                                            Buteo::PluginCbInterface* cbInterface) override;
};


#endif // GOOGLECALENDARSPLUGIN_H
