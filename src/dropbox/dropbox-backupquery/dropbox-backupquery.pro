TARGET = dropbox-backupquery-client

DEFINES += "CLASSNAME=DropboxBackupQueryPlugin"
DEFINES += CLASSNAME_H=\\\"dropboxbackupqueryplugin.h\\\"
include($$PWD/../../common.pri)
include($$PWD/../dropbox-common.pri)
include($$PWD/../dropbox-backupoperation.pri)
include($$PWD/dropbox-backupquery.pri)

dropbox_backupquery_sync_profile.path = /etc/buteo/profiles/sync
dropbox_backupquery_sync_profile.files = $$PWD/dropbox.BackupQuery.xml
dropbox_backupquery_client_plugin_xml.path = /etc/buteo/profiles/client
dropbox_backupquery_client_plugin_xml.files = $$PWD/dropbox-backupquery.xml

HEADERS += dropboxbackupqueryplugin.h
SOURCES += dropboxbackupqueryplugin.cpp

OTHER_FILES += \
    dropbox_backupquery_sync_profile.files \
    dropbox_backupquery_client_plugin_xml.files

INSTALLS += \
    target \
    dropbox_backupquery_sync_profile \
    dropbox_backupquery_client_plugin_xml
