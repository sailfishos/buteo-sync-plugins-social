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

#include "facebookcalendarsplugin.h"
#include "facebookcalendarsyncadaptor.h"
#include "socialnetworksyncadaptor.h"

FacebookCalendarsPlugin::FacebookCalendarsPlugin(const QString& pluginName,
                             const Buteo::SyncProfile& profile,
                             Buteo::PluginCbInterface *callbackInterface)
    : SocialdButeoPlugin(pluginName, profile, callbackInterface,
                         QStringLiteral("facebook"),
                         SocialNetworkSyncAdaptor::dataTypeName(SocialNetworkSyncAdaptor::Calendars))
{
}

FacebookCalendarsPlugin::~FacebookCalendarsPlugin()
{
}

SocialNetworkSyncAdaptor *FacebookCalendarsPlugin::createSocialNetworkSyncAdaptor()
{
    return new FacebookCalendarSyncAdaptor(this);
}


Buteo::ClientPlugin* FacebookCalendarsPluginLoader::createClientPlugin(
        const QString& pluginName,
        const Buteo::SyncProfile& profile,
        Buteo::PluginCbInterface* cbInterface)
{
    return new FacebookCalendarsPlugin(pluginName, profile, cbInterface);
}
