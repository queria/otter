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

#include "QtWebKitNetworkManager.h"
#include "QtWebKitWebWidget.h"
#include "../../../../core/ContentBlockingManager.h"
#include "../../../../core/Console.h"
#include "../../../../core/CookieJar.h"
#include "../../../../core/LocalListingNetworkReply.h"
#include "../../../../core/NetworkManagerFactory.h"
#include "../../../../core/SettingsManager.h"
#include "../../../../core/Utils.h"
#include "../../../../ui/AuthenticationDialog.h"
#include "../../../../ui/ContentsDialog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>

namespace Otter
{

QtWebKitNetworkManager::QtWebKitNetworkManager(bool isPrivate, QtWebKitWebWidget *widget) : NetworkManager(isPrivate, widget),
	m_widget(widget),
	m_baseReply(NULL),
	m_speed(0),
	m_bytesReceivedDifference(0),
	m_bytesReceived(0),
	m_bytesTotal(0),
	m_finishedRequests(0),
	m_startedRequests(0),
	m_updateTimer(0),
	m_doNotTrackPolicy(NetworkManagerFactory::SkipTrackPolicy),
	m_canSendReferrer(true)
{
	connect(this, SIGNAL(finished(QNetworkReply*)), SLOT(requestFinished(QNetworkReply*)));
}

void QtWebKitNetworkManager::timerEvent(QTimerEvent *event)
{
	Q_UNUSED(event)

	updateStatus();
}

void QtWebKitNetworkManager::handleAuthenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator)
{
	emit messageChanged(tr("Waiting for authentication…"));

	AuthenticationDialog *authenticationDialog = new AuthenticationDialog(reply->url(), authenticator, m_widget);
	authenticationDialog->setButtonsVisible(false);

	ContentsDialog dialog(Utils::getIcon(QLatin1String("dialog-password")), authenticationDialog->windowTitle(), QString(), QString(), (QDialogButtonBox::Ok | QDialogButtonBox::Cancel), authenticationDialog, m_widget);

	connect(&dialog, SIGNAL(accepted()), authenticationDialog, SLOT(accept()));

	QEventLoop eventLoop;

	m_widget->showDialog(&dialog);

	connect(&dialog, SIGNAL(closed(bool,QDialogButtonBox::StandardButton)), &eventLoop, SLOT(quit()));
	connect(m_widget, SIGNAL(aboutToReload()), &eventLoop, SLOT(quit()));
	connect(this, SIGNAL(destroyed()), &eventLoop, SLOT(quit()));

	eventLoop.exec();

	m_widget->hideDialog(&dialog);
}

void QtWebKitNetworkManager::handleProxyAuthenticationRequired(const QNetworkProxy &proxy, QAuthenticator *authenticator)
{
	if (NetworkManagerFactory::isUsingSystemProxyAuthentication())
	{
		authenticator->setUser(QString());

		return;
	}

	emit messageChanged(tr("Waiting for authentication…"));

	AuthenticationDialog *authenticationDialog = new AuthenticationDialog(proxy.hostName(), authenticator, m_widget);
	authenticationDialog->setButtonsVisible(false);

	ContentsDialog dialog(Utils::getIcon(QLatin1String("dialog-password")), authenticationDialog->windowTitle(), QString(), QString(), (QDialogButtonBox::Ok | QDialogButtonBox::Cancel), authenticationDialog, m_widget);

	connect(&dialog, SIGNAL(accepted()), authenticationDialog, SLOT(accept()));

	QEventLoop eventLoop;

	m_widget->showDialog(&dialog);

	connect(&dialog, SIGNAL(closed(bool,QDialogButtonBox::StandardButton)), &eventLoop, SLOT(quit()));
	connect(m_widget, SIGNAL(aboutToReload()), &eventLoop, SLOT(quit()));
	connect(this, SIGNAL(destroyed()), &eventLoop, SLOT(quit()));

	eventLoop.exec();

	m_widget->hideDialog(&dialog);
}

void QtWebKitNetworkManager::handleSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
	if (errors.isEmpty())
	{
		reply->ignoreSslErrors(errors);

		return;
	}

	QStringList ignoredErrors = m_widget->getOption(QLatin1String("Security/IgnoreSslErrors"), m_widget->getUrl()).toStringList();
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
			else if (shouldHandleSelfSignedError(errors.at(i), m_widget->getUrl()))
			{ // if this is self-signed case and we can handle it ...
				QString oldSelfSignedHash = getKnownSelfSignedHash(m_widget->getUrl());
				QString newSelfSignedHash = errors.at(i).certificate().digest(QCryptographicHash::Sha1).toHex();

				if (oldSelfSignedHash.isEmpty())
				{ // first time we see this self-signed page/cert
					setKnownSelfSignedHash(m_widget->getUrl(), newSelfSignedHash);
					errorsToIgnore.append(errors.at(i));
				}
				else if (oldSelfSignedHash == newSelfSignedHash)
				{ // same cert as last time
					errorsToIgnore.append(errors.at(i));
				}
				else
				{ // cert has changed
					messages.append(tr("Self-signed certificate for %1 has changed!\n\nSHA1 fingerprints:\n old: %2\n new: %3!")
							.arg(m_widget->getUrl().toString(QUrl::RemoveUserInfo | QUrl::RemovePath))
							.arg(oldSelfSignedHash)
							.arg(newSelfSignedHash));
				}
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

	ContentsDialog dialog(Utils::getIcon(QLatin1String("dialog-warning")), tr("Warning"), tr("SSL errors occured, do you want to continue?"), messages.join('\n'), (QDialogButtonBox::Yes | QDialogButtonBox::No), NULL, m_widget);

	if (!m_widget->getUrl().isEmpty())
	{
		dialog.setCheckBox(tr("Do not show this message again"), false);
	}

	QEventLoop eventLoop;

	m_widget->showDialog(&dialog);

	connect(&dialog, SIGNAL(closed(bool,QDialogButtonBox::StandardButton)), &eventLoop, SLOT(quit()));
	connect(m_widget, SIGNAL(aboutToReload()), &eventLoop, SLOT(quit()));
	connect(this, SIGNAL(destroyed()), &eventLoop, SLOT(quit()));

	eventLoop.exec();

	m_widget->hideDialog(&dialog);

	if (dialog.isAccepted())
	{
		reply->ignoreSslErrors(errors);

		if (!m_widget->getUrl().isEmpty() && dialog.getCheckBoxState())
		{
			for (int i = 0; i < errors.count(); ++i)
			{
				if(shouldHandleSelfSignedError(errors.at(i), m_widget->getUrl())) {
					setKnownSelfSignedHash(
							m_widget->getUrl(),
							errors.at(i).certificate().digest(QCryptographicHash::Sha1).toHex());

				} else {
					const QString digest = errors.at(i).certificate().digest().toBase64();

					if (!ignoredErrors.contains(digest))
					{
						ignoredErrors.append(digest);
					}
				}
			}

			if (!ignoredErrors.isEmpty())
			{
				SettingsManager::setValue(QLatin1String("Security/IgnoreSslErrors"), ignoredErrors, m_widget->getUrl());
			}
		}
	}
}

void QtWebKitNetworkManager::resetStatistics()
{
	killTimer(m_updateTimer);
	updateStatus();

	m_updateTimer = 0;
	m_replies.clear();
	m_baseReply = NULL;
	m_speed = 0;
	m_bytesReceivedDifference = 0;
	m_bytesReceived = 0;
	m_bytesTotal = 0;
	m_finishedRequests = 0;
	m_startedRequests = 0;
}

void QtWebKitNetworkManager::downloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
	QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

	if (reply && reply == m_baseReply)
	{
		if (m_baseReply->hasRawHeader(QStringLiteral("Location").toLatin1()))
		{
			m_baseReply = NULL;
		}
		else
		{
			if (bytesTotal > 0)
			{
				emit documentLoadProgressChanged(((bytesReceived * 1.0) / bytesTotal) * 100);
			}
			else
			{
				emit documentLoadProgressChanged(-1);
			}
		}
	}

	if (!reply || !m_replies.contains(reply))
	{
		return;
	}

	emit messageChanged(tr("Receiving data from %1…").arg(reply->url().host()));

	const qint64 difference = (bytesReceived - m_replies[reply].first);

	m_replies[reply].first = bytesReceived;

	if (!m_replies[reply].second && bytesTotal > 0)
	{
		m_replies[reply].second = true;

		m_bytesTotal += bytesTotal;
	}

	if (difference <= 0)
	{
		return;
	}

	m_bytesReceived += difference;
	m_bytesReceivedDifference += difference;
}

void QtWebKitNetworkManager::requestFinished(QNetworkReply *reply)
{
	m_replies.remove(reply);

	if (m_replies.isEmpty())
	{
		killTimer(m_updateTimer);

		m_updateTimer = 0;

		updateStatus();
	}

	++m_finishedRequests;

	if (reply)
	{
		emit messageChanged(tr("Completed request to %1").arg(reply->url().host()));

		disconnect(reply, SIGNAL(downloadProgress(qint64,qint64)), this, SLOT(downloadProgress(qint64,qint64)));
	}
}

void QtWebKitNetworkManager::updateStatus()
{
	m_speed = (m_bytesReceivedDifference * 2);
	m_bytesReceivedDifference = 0;

	emit statusChanged(m_finishedRequests, m_startedRequests, m_bytesReceived, m_bytesTotal, m_speed);
}

void QtWebKitNetworkManager::updateOptions(const QUrl &url)
{
	QString acceptLanguage = SettingsManager::getValue(QLatin1String("Network/AcceptLanguage"), url).toString();
	acceptLanguage = ((acceptLanguage.isEmpty()) ? QLatin1String(" ") : acceptLanguage.replace(QLatin1String("system"), QLocale::system().bcp47Name()));

	m_acceptLanguage = ((acceptLanguage == NetworkManagerFactory::getAcceptLanguage()) ? QString() : acceptLanguage);

	const QString policyValue = SettingsManager::getValue(QLatin1String("Network/DoNotTrackPolicy"), url).toString();

	if (policyValue == QLatin1String("allow"))
	{
		m_doNotTrackPolicy = NetworkManagerFactory::AllowToTrackPolicy;
	}
	else if (policyValue == QLatin1String("doNotAllow"))
	{
		m_doNotTrackPolicy = NetworkManagerFactory::DoNotAllowToTrackPolicy;
	}
	else
	{
		m_doNotTrackPolicy = NetworkManagerFactory::SkipTrackPolicy;
	}

	m_canSendReferrer = SettingsManager::getValue(QLatin1String("Network/EnableReferrer"), url).toBool();
}

void QtWebKitNetworkManager::setFormRequest(const QUrl &url)
{
	m_formRequestUrl = url;
}

void QtWebKitNetworkManager::setWidget(QtWebKitWebWidget *widget)
{
	m_widget = widget;
}

QtWebKitNetworkManager* QtWebKitNetworkManager::clone()
{
	QtWebKitNetworkManager *manager = new QtWebKitNetworkManager((cache() == NULL), NULL);
	manager->setCookieJar(getCookieJar()->clone(manager));

	return manager;
}

QNetworkReply* QtWebKitNetworkManager::createRequest(QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *outgoingData)
{
	if (request.url() == m_formRequestUrl)
	{
		m_formRequestUrl = QUrl();

		m_widget->openFormRequest(request.url(), operation, outgoingData);

		return QNetworkAccessManager::createRequest(QNetworkAccessManager::GetOperation, QNetworkRequest());
	}

	++m_startedRequests;

	if (ContentBlockingManager::isContentBlockingEnabled() && ContentBlockingManager::isUrlBlocked(request, m_widget->getUrl()))
	{
		Console::addMessage(QCoreApplication::translate("main", "Blocked content: %0").arg(request.url().url()), Otter::NetworkMessageCategory, LogMessageLevel);

		QUrl url = QUrl();
		url.setScheme(QLatin1String("http"));

		return QNetworkAccessManager::createRequest(QNetworkAccessManager::GetOperation, QNetworkRequest(url));
	}

	if (operation == GetOperation && request.url().isLocalFile() && QFileInfo(request.url().toLocalFile()).isDir())
	{
		return new LocalListingNetworkReply(this, request);
	}

	QNetworkRequest mutableRequest(request);

	if (!m_canSendReferrer)
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
	else if (m_doNotTrackPolicy != NetworkManagerFactory::SkipTrackPolicy)
	{
		mutableRequest.setRawHeader(QByteArray("DNT"), QByteArray((m_doNotTrackPolicy == NetworkManagerFactory::DoNotAllowToTrackPolicy) ? "1" : "0"));
	}

	mutableRequest.setRawHeader(QStringLiteral("Accept-Language").toLatin1(), (m_acceptLanguage.isEmpty() ? NetworkManagerFactory::getAcceptLanguage().toLatin1() : m_acceptLanguage.toLatin1()));

	emit messageChanged(tr("Sending request to %1…").arg(request.url().host()));

	QNetworkReply *reply = QNetworkAccessManager::createRequest(operation, mutableRequest, outgoingData);

	if (!m_baseReply)
	{
		m_baseReply = reply;
	}

	m_replies[reply] = qMakePair(0, false);

	connect(reply, SIGNAL(downloadProgress(qint64,qint64)), this, SLOT(downloadProgress(qint64,qint64)));

	if (m_updateTimer == 0)
	{
		m_updateTimer = startTimer(500);
	}

	return reply;
}

QHash<QByteArray, QByteArray> QtWebKitNetworkManager::getHeaders() const
{
	QHash<QByteArray, QByteArray> headers;

	if (m_baseReply)
	{
		const QList<QNetworkReply::RawHeaderPair> rawHeaders = m_baseReply->rawHeaderPairs();

		for (int i = 0; i < rawHeaders.count(); ++i)
		{
			headers[rawHeaders.at(i).first] = rawHeaders.at(i).second;
		}
	}

	return headers;
}

QVariantHash QtWebKitNetworkManager::getStatistics() const
{
	QVariantHash statistics;
	statistics[QLatin1String("finishedRequests")] = m_finishedRequests;
	statistics[QLatin1String("startedRequests")] = m_startedRequests;
	statistics[QLatin1String("bytesReceived")] = m_bytesReceived;
	statistics[QLatin1String("bytesTotal")] = m_bytesTotal;
	statistics[QLatin1String("speed")] = m_speed;

	return statistics;
}

}
