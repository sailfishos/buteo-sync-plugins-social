TARGET = vk-notifications-client

include($$PWD/../../common.pri)
include($$PWD/../vk-common.pri)
include($$PWD/vk-notifications.pri)

vk_notifications_sync_profile.path = /etc/buteo/profiles/sync
vk_notifications_sync_profile.files = $$PWD/vk.Notifications.xml
vk_notifications_client_plugin_xml.path = /etc/buteo/profiles/client
vk_notifications_client_plugin_xml.files = $$PWD/vk-notifications.xml

HEADERS += vknotificationsplugin.h
SOURCES += vknotificationsplugin.cpp

OTHER_FILES += \
    vk.Notifications.xml \
    vk-notifications.xml

INSTALLS += \
    target \
    vk_notifications_sync_profile \
    vk_notifications_client_plugin_xml
