/****************************************************************************
 **
 ** Copyright (C) 2014 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
 **
 ****************************************************************************/

#include "vkcontactimagedownloader.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkAccessManager>

static const char *IMAGE_DOWNLOADER_IDENTIFIER_KEY = "identifier";

VKContactImageDownloader::VKContactImageDownloader()
    : AbstractImageDownloader()
{
}

QString VKContactImageDownloader::staticOutputFile(const QString &identifier, const QUrl &url)
{
    return makeOutputFile(SocialSyncInterface::VK, SocialSyncInterface::Contacts, identifier, url.toString());
}

QNetworkReply * VKContactImageDownloader::createReply(const QString &url,
                                                      const QVariantMap &metadata)
{
    Q_D(AbstractImageDownloader);

    Q_UNUSED(metadata)

    QNetworkRequest request(url);
    return d->networkAccessManager->get(request);
}

QString VKContactImageDownloader::outputFile(const QString &url, const QVariantMap &data) const
{
    return staticOutputFile(data.value(IMAGE_DOWNLOADER_IDENTIFIER_KEY).toString(), url);
}
