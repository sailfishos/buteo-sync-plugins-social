/****************************************************************************
 **
 ** Copyright (C) 2014 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jolla.com>
 **
 ****************************************************************************/

#ifndef VKCONTACTSPLUGIN_H
#define VKCONTACTSPLUGIN_H

#include "socialdbuteoplugin.h"

#include <buteosyncfw5/SyncPluginLoader.h>

class Q_DECL_EXPORT VKContactsPlugin : public SocialdButeoPlugin
{
    Q_OBJECT

public:
    VKContactsPlugin(const QString& pluginName,
                  const Buteo::SyncProfile& profile,
                  Buteo::PluginCbInterface *cbInterface);
    ~VKContactsPlugin();

protected:
    SocialNetworkSyncAdaptor *createSocialNetworkSyncAdaptor() override;
};

class VKContactsPluginLoader : public Buteo::SyncPluginLoader
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.sailfishos.plugins.sync.VKContactsPluginLoader")
    Q_INTERFACES(Buteo::SyncPluginLoader)

public:
    Buteo::ClientPlugin* createClientPlugin(const QString& pluginName,
                                            const Buteo::SyncProfile& profile,
                                            Buteo::PluginCbInterface* cbInterface) override;
};

#endif // VKCONTACTSPLUGIN_H
