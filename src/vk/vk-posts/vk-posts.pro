TARGET = vk-posts-client

DEFINES += "CLASSNAME=VKPostsPlugin"
DEFINES += CLASSNAME_H=\\\"vkpostsplugin.h\\\"
DEFINES += SOCIALD_USE_QTPIM
include($$PWD/../../common.pri)
include($$PWD/../vk-common.pri)
include($$PWD/vk-posts.pri)

PKGCONFIG += mlite5

vk_posts_sync_profile.path = /etc/buteo/profiles/sync
vk_posts_sync_profile.files = $$PWD/vk.Posts.xml
vk_posts_client_plugin_xml.path = /etc/buteo/profiles/client
vk_posts_client_plugin_xml.files = $$PWD/vk-posts.xml

HEADERS += vkpostsplugin.h
SOURCES += vkpostsplugin.cpp

OTHER_FILES += \
    vk.Posts.xml \
    vk-posts.xml

INSTALLS += \
    target \
    vk_posts_sync_profile \
    vk_posts_client_plugin_xml
