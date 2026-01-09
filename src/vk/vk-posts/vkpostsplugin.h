/****************************************************************************
 **
 ** Copyright (C) 2014 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jolla.com>
 **
 ****************************************************************************/

#ifndef VKPOSTSPLUGIN_H
#define VKPOSTSPLUGIN_H

#include "socialdbuteoplugin.h"

#include <buteosyncfw5/SyncPluginLoader.h>

class Q_DECL_EXPORT VKPostsPlugin : public SocialdButeoPlugin
{
    Q_OBJECT

public:
    VKPostsPlugin(const QString& pluginName,
                  const Buteo::SyncProfile& profile,
                  Buteo::PluginCbInterface *cbInterface);
    ~VKPostsPlugin();

protected:
    SocialNetworkSyncAdaptor *createSocialNetworkSyncAdaptor() override;
};

class VKPostsPluginLoader : public Buteo::SyncPluginLoader
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.sailfishos.plugins.sync.VKPostsPluginLoader")
    Q_INTERFACES(Buteo::SyncPluginLoader)

public:
    Buteo::ClientPlugin* createClientPlugin(const QString& pluginName,
                                            const Buteo::SyncProfile& profile,
                                            Buteo::PluginCbInterface* cbInterface) override;
};

#endif // VKPOSTSPLUGIN_H
