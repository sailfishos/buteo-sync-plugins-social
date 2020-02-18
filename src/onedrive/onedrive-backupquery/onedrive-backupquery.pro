TARGET = onedrive-backupquery-client

DEFINES += "CLASSNAME=OneDriveBackupQueryPlugin"
DEFINES += CLASSNAME_H=\\\"onedrivebackupqueryplugin.h\\\"
include($$PWD/../../common.pri)
include($$PWD/../onedrive-common.pri)
include($$PWD/../onedrive-backupoperation.pri)
include($$PWD/onedrive-backupquery.pri)

onedrive_backupquery_sync_profile.path = /etc/buteo/profiles/sync
onedrive_backupquery_sync_profile.files = $$PWD/onedrive.BackupQuery.xml
onedrive_backupquery_client_plugin_xml.path = /etc/buteo/profiles/client
onedrive_backupquery_client_plugin_xml.files = $$PWD/onedrive-backupquery.xml

HEADERS += onedrivebackupqueryplugin.h
SOURCES += onedrivebackupqueryplugin.cpp

OTHER_FILES += \
    onedrive_backupquery_sync_profile.files \
    onedrive_backupquery_client_plugin_xml.files

INSTALLS += \
    target \
    onedrive_backupquery_sync_profile \
    onedrive_backupquery_client_plugin_xml
