/**************************************************************************
* Otter Browser: Web browser controlled by the user, not vice-versa.
* Copyright (C) 2013 - 2014 Michal Dutkiewicz aka Emdek <michal@emdek.pl>
* Copyright (C) 2014 Piotr Wójcik <chocimier@tlen.pl>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
**************************************************************************/

#include "NetworkManager.h"
#include "CookieJar.h"
#include "LocalListingNetworkReply.h"
#include "NetworkCache.h"
#include "NetworkManagerFactory.h"
#include "SessionsManager.h"
#include "SettingsManager.h"
#include "Utils.h"
#include "../ui/AuthenticationDialog.h"
#include "../ui/MainWindow.h"

#include <QtCore/QFileInfo>
#include <QtWidgets/QMessageBox>
#include <QtNetwork/QNetworkProxy>

namespace Otter
{

NetworkManager::NetworkManager(bool isPrivate, QObject *parent) : QNetworkAccessManager(parent),
	m_cookieJar(NULL)
{
	NetworkManagerFactory::initialize();

	if (!isPrivate)
	{
		m_cookieJar = NetworkManagerFactory::getCookieJar();

		setCookieJar(m_cookieJar);

		m_cookieJar->setParent(QCoreApplication::instance());

		QNetworkDiskCache *cache = NetworkManagerFactory::getCache();

		setCache(cache);

		cache->setParent(QCoreApplication::instance());
	}
	else
	{
		m_cookieJar = new CookieJar(true, this);

		setCookieJar(m_cookieJar);
	}

	connect(this, SIGNAL(authenticationRequired(QNetworkReply*,QAuthenticator*)), this, SLOT(handleAuthenticationRequired(QNetworkReply*,QAuthenticator*)));
	connect(this, SIGNAL(proxyAuthenticationRequired(QNetworkProxy,QAuthenticator*)), this, SLOT(handleProxyAuthenticationRequired(QNetworkProxy,QAuthenticator*)));
	connect(this, SIGNAL(sslErrors(QNetworkReply*,QList<QSslError>)), this, SLOT(handleSslErrors(QNetworkReply*,QList<QSslError>)));
}

void NetworkManager::handleAuthenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator)
{
	AuthenticationDialog dialog(reply->url(), authenticator, SessionsManager::getActiveWindow());
	dialog.exec();
}

void NetworkManager::handleProxyAuthenticationRequired(const QNetworkProxy &proxy, QAuthenticator *authenticator)
{
	if (NetworkManagerFactory::isUsingSystemProxyAuthentication())
	{
		authenticator->setUser(QString());

		return;
	}

	AuthenticationDialog dialog(QUrl(proxy.hostName()), authenticator, SessionsManager::getActiveWindow());
	dialog.exec();
}

void NetworkManager::handleSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
	if (errors.isEmpty())
	{
		reply->ignoreSslErrors(errors);

		return;
	}

	QStringList ignoredErrors = SettingsManager::getValue(QLatin1String("Security/IgnoreSslErrors"), reply->url()).toStringList();
	QStringList messages;
	QList<QSslError> errorsToIgnore;

	for (int i = 0; i < errors.count(); ++i)
	{
		if (errors.at(i).error() != QSslError::NoError)
		{
			if (ignoredErrors.contains(errors.at(i).certificate().digest().toBase64()))
			{
				errorsToIgnore.append(errors.at(i));
			}
			else
			{
				messages.append(errors.at(i).errorString());
			}
		}
	}

	if (!errorsToIgnore.isEmpty())
	{
		reply->ignoreSslErrors(errorsToIgnore);
	}

	if (messages.isEmpty())
	{
		return;
	}

	if (QMessageBox::warning(SessionsManager::getActiveWindow(), tr("Warning"), tr("SSL errors occured:\n\n%1\n\nDo you want to continue?").arg(messages.join('\n')), (QMessageBox::Yes | QMessageBox::No)) == QMessageBox::Yes)
	{
		reply->ignoreSslErrors(errors);
	}
}

bool NetworkManager::shouldHandleSelfSignedError(QSslError error, QUrl url)
{
	return (!url.isEmpty()
			&& SettingsManager::getValue(QLatin1String("Security/AcceptSelfSignedCerts")).toBool()
			&& (error.error() == QSslError::SelfSignedCertificate
				|| error.error() == QSslError::SelfSignedCertificateInChain));
}

QString NetworkManager::getKnownSelfSignedHash(QUrl url)
{
	QStringList known = SettingsManager::getValue(QLatin1String("Security/KnownSelfSigned"), url).toStringList();
	QString portIdent = QString("%1:").arg(url.port(443));

	for(int i = 0; i < known.count(); ++i)
	{
	  if(known.at(i).startsWith(portIdent)) {
		  return known.at(i).right(known.at(i).length() - portIdent.length());
	  }
	}
	return "";
}

void NetworkManager::setKnownSelfSignedHash(QUrl url, QString hash)
{
	QStringList known = SettingsManager::getValue(QLatin1String("Security/KnownSelfSigned"), url).toStringList();
	QString portIdent = QString("%1:").arg(url.port(443));

	for(int i = 0; i < known.count(); ++i)
	{
		if(known.at(i).startsWith(portIdent)) {
			known.removeAt(i);
			break;
		}
	}
	known << (portIdent + hash);

	SettingsManager::setValue(QLatin1String("Security/KnownSelfSigned"), known, url);
}

CookieJar* NetworkManager::getCookieJar()
{
	return m_cookieJar;
}

QNetworkReply* NetworkManager::createRequest(QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *outgoingData)
{
	if (operation == GetOperation && request.url().isLocalFile() && QFileInfo(request.url().toLocalFile()).isDir())
	{
		return new LocalListingNetworkReply(this, request);
	}

	QNetworkRequest mutableRequest(request);

	if (!NetworkManagerFactory::canSendReferrer())
	{
		mutableRequest.setRawHeader(QStringLiteral("Referer").toLatin1(), QByteArray());
	}

	if (operation == PostOperation && mutableRequest.header(QNetworkRequest::ContentTypeHeader).isNull())
	{
		mutableRequest.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/x-www-form-urlencoded"));
	}

	if (NetworkManagerFactory::isWorkingOffline())
	{
		mutableRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysCache);
	}
	else if (NetworkManagerFactory::getDoNotTrackPolicy() != NetworkManagerFactory::SkipTrackPolicy)
	{
		mutableRequest.setRawHeader(QByteArray("DNT"), QByteArray((NetworkManagerFactory::getDoNotTrackPolicy() == NetworkManagerFactory::DoNotAllowToTrackPolicy) ? "1" : "0"));
	}

	mutableRequest.setRawHeader(QStringLiteral("Accept-Language").toLatin1(), NetworkManagerFactory::getAcceptLanguage().toLatin1());

	return QNetworkAccessManager::createRequest(operation, mutableRequest, outgoingData);
}

}
