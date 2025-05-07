Name:       buteo-sync-plugins-social
Summary:    Sync plugins for social services
Version:    0.4.0
Release:    1
License:    LGPLv2
URL:        https://github.com/sailfishos/buteo-sync-plugins-social/
Source0:    %{name}-%{version}.tar.bz2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  qt5-qttools-linguist
BuildRequires:  pkgconfig(mlite5)
BuildRequires:  pkgconfig(buteosyncfw5) >= 0.10.0
BuildRequires:  pkgconfig(libsignon-qt5)
BuildRequires:  pkgconfig(accounts-qt5) >= 1.13
BuildRequires:  pkgconfig(socialcache) >= 0.0.48
BuildRequires:  pkgconfig(libsailfishkeyprovider)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions) >= 0.3.0
BuildRequires:  pkgconfig(libmkcal-qt5) >= 0.5.45
BuildRequires:  pkgconfig(KF5CalendarCore) >= 5.79
BuildRequires:  nemo-qml-plugin-notifications-qt5-devel
Requires: buteo-syncfw-qt5-msyncd
Requires: systemd
Requires(pre):  sailfish-setup
Requires(post): systemd

%description
A Buteo plugin which provides data synchronization with various social services.

%package facebook
Summary:    Provides synchronisation with Facebook
Requires: %{name} = %{version}-%{release}

%description facebook
%{summary}.

%package google
Summary:    Provides synchronisation with Google
Requires: %{name} = %{version}-%{release}

%description google
%{summary}.

%package twitter
Summary:    Provides synchronisation with Twitter
Requires: %{name} = %{version}-%{release}

%description twitter
%{summary}.

%package onedrive
Summary:    Provides synchronisation with OneDrive
Requires: %{name} = %{version}-%{release}

%description onedrive
%{summary}.

%package vk
Summary:    Provides synchronisation with VK
Requires: %{name} = %{version}-%{release}

%description vk
%{summary}.

%package dropbox
Summary:    Provides synchronisation with Dropbox
Requires: %{name} = %{version}-%{release}

%description dropbox
%{summary}.

%package knowncontacts
Summary: Store locally created contacts
Requires: %{name} = %{version}-%{release}

%description knowncontacts
Buteo sync plugin that stores locally created contacts, such as email
recipients.

%package ts-devel
Summary:    Translation source for sociald

%description ts-devel
%{summary}.

%prep
%setup -q -n %{name}-%{version}

%build
%qmake5 \
    "CONFIG+=dropbox" \
    "CONFIG+=facebook" \
    "CONFIG+=google" \
    "CONFIG+=onedrive" \
    "CONFIG+=twitter" \
    "CONFIG+=vk" \
    "CONFIG+=knowncontacts" \
    "CONFIG+=calendar"

%make_build

%install
%qmake5_install

%pre
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/sociald.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.facebook.Calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.facebook.Contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.facebook.Images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.facebook.Notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.twitter.Notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.twitter.Posts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.google.Calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.google.Contacts.xml || :
done

%pre facebook
# calendar
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Calendars.xml || :
done
#images
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Images.xml || :
done
#signon
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Signon.xml || :
done

%pre google
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
# calendar
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Calendars.xml || :
done
# contacts
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Contacts.xml || :
done
# signon
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Signon.xml || :
done

%pre twitter
# notifications
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/twitter-notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/twitter.Notifications.xml || :
done
# posts
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/twitter-posts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/twitter.Posts.xml || :
done

%pre onedrive
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
# signon
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Signon.xml || :
done
# images
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Images.xml || :
done
# backup
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backupquery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backuprestore.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.BackupQuery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.BackupRestore.xml || :
done

%pre vk
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
# posts
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-posts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Posts.xml || :
done
# notifications
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Notifications.xml || :
done
# calendars
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Calendars.xml || :
done
# contacts
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Contacts.xml || :
done
# images
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Images.xml || :
done

%pre dropbox
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
# images
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.Images.xml || :
done
# backup
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backupquery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backuprestore.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.Backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.BackupQuery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.BackupRestore.xml || :
done

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%{_libdir}/buteo-plugins-qt5/oopp/libsociald-client.so
%config %{_sysconfdir}/buteo/profiles/client/sociald.xml
%config %{_sysconfdir}/buteo/profiles/sync/sociald.All.xml
%{_libdir}/libsyncpluginscommon.so.*
%exclude %{_libdir}/libsyncpluginscommon.so
%license COPYING

%files facebook
# calendar:
%{_libdir}/buteo-plugins-qt5/oopp/libfacebook-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Calendars.xml
# images:
%{_libdir}/buteo-plugins-qt5/oopp/libfacebook-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Images.xml
# signon
%{_libdir}/buteo-plugins-qt5/oopp/libfacebook-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Signon.xml

%files google
# calendar
%{_libdir}/buteo-plugins-qt5/oopp/libgoogle-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Calendars.xml
# contacts
%{_libdir}/buteo-plugins-qt5/oopp/libgoogle-contacts-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-contacts.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Contacts.xml
# signon
%{_libdir}/buteo-plugins-qt5/oopp/libgoogle-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Signon.xml

%files twitter
# notifications
%{_libdir}/buteo-plugins-qt5/oopp/libtwitter-notifications-client.so
%config %{_sysconfdir}/buteo/profiles/client/twitter-notifications.xml
%config %{_sysconfdir}/buteo/profiles/sync/twitter.Notifications.xml
%{_datadir}/translations/lipstick-jolla-home-twitter-notif_eng_en.qm
# posts
%{_libdir}/buteo-plugins-qt5/oopp/libtwitter-posts-client.so
%config %{_sysconfdir}/buteo/profiles/client/twitter-posts.xml
%config %{_sysconfdir}/buteo/profiles/sync/twitter.Posts.xml

%files onedrive
# signon
%{_libdir}/buteo-plugins-qt5/oopp/libonedrive-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/onedrive-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.Signon.xml
# images
%{_libdir}/buteo-plugins-qt5/oopp/libonedrive-images-client.so
# backup
%config %{_sysconfdir}/buteo/profiles/client/onedrive-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.Images.xml
%{_libdir}/buteo-plugins-qt5/oopp/libonedrive-backup-client.so
%{_libdir}/buteo-plugins-qt5/oopp/libonedrive-backupquery-client.so
%{_libdir}/buteo-plugins-qt5/oopp/libonedrive-backuprestore-client.so
#%%{_libdir}/buteo-plugins-qt5/libonedrive-backupquery-client.so
#%%{_libdir}/buteo-plugins-qt5/libonedrive-backuprestore-client.so
%config %{_sysconfdir}/buteo/profiles/client/onedrive-backup.xml
%config %{_sysconfdir}/buteo/profiles/client/onedrive-backupquery.xml
%config %{_sysconfdir}/buteo/profiles/client/onedrive-backuprestore.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.Backup.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.BackupQuery.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.BackupRestore.xml

%files vk
# posts
%{_libdir}/buteo-plugins-qt5/oopp/libvk-posts-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-posts.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Posts.xml
# notifications
%{_libdir}/buteo-plugins-qt5/oopp/libvk-notifications-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-notifications.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Notifications.xml
# calendars
#out-of-proces-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/libvk-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Calendars.xml
# contacts
%{_libdir}/buteo-plugins-qt5/oopp/libvk-contacts-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-contacts.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Contacts.xml
# images
%{_libdir}/buteo-plugins-qt5/oopp/libvk-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Images.xml

%files dropbox
# images
%{_libdir}/buteo-plugins-qt5/oopp/libdropbox-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/dropbox-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.Images.xml
# backup
%{_libdir}/buteo-plugins-qt5/oopp/libdropbox-backup-client.so
%{_libdir}/buteo-plugins-qt5/oopp/libdropbox-backupquery-client.so
%{_libdir}/buteo-plugins-qt5/oopp/libdropbox-backuprestore-client.so
#%%{_libdir}/buteo-plugins-qt5/libdropbox-backupquery-client.so
#%%{_libdir}/buteo-plugins-qt5/libdropbox-backuprestore-client.so
%config %{_sysconfdir}/buteo/profiles/client/dropbox-backup.xml
%config %{_sysconfdir}/buteo/profiles/client/dropbox-backupquery.xml
%config %{_sysconfdir}/buteo/profiles/client/dropbox-backuprestore.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.Backup.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.BackupQuery.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.BackupRestore.xml

%files knowncontacts
%{_libdir}/buteo-plugins-qt5/oopp/libknowncontacts-client.so
%{_sysconfdir}/buteo/profiles/client/knowncontacts.xml
%{_sysconfdir}/buteo/profiles/sync/knowncontacts.Contacts.xml

%files ts-devel
%{_datadir}/translations/source/lipstick-jolla-home-twitter-notif.ts
