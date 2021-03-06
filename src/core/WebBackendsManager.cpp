/**************************************************************************
* Otter Browser: Web browser controlled by the user, not vice-versa.
* Copyright (C) 2013 - 2015 Michal Dutkiewicz aka Emdek <michal@emdek.pl>
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

#include "WebBackendsManager.h"
#include "SettingsManager.h"
#include "WebBackend.h"
#ifdef OTTER_ENABLE_QTWEBENGINE
#include "../modules/backends/web/qtwebengine/QtWebEngineWebBackend.h"
#endif
#include "../modules/backends/web/qtwebkit/QtWebKitWebBackend.h"

namespace Otter
{

WebBackendsManager *WebBackendsManager::m_instance = NULL;
QHash<QString, WebBackend*> WebBackendsManager::m_backends;

WebBackendsManager::WebBackendsManager(QObject *parent) : QObject(parent)
{
#ifdef OTTER_ENABLE_QTWEBENGINE
	registerBackend(new QtWebEngineWebBackend(this), QLatin1String("qtwebengine"));
#endif
	registerBackend(new QtWebKitWebBackend(this), QLatin1String("qtwebkit"));
}

void WebBackendsManager::createInstance(QObject *parent)
{
	if (!m_instance)
	{
		m_instance = new WebBackendsManager(parent);
	}
}

void WebBackendsManager::registerBackend(WebBackend *backend, const QString &name)
{
	m_backends[name] = backend;
}

QStringList WebBackendsManager::getBackends()
{
	return m_backends.keys();
}

WebBackend* WebBackendsManager::getBackend(const QString &name)
{
	if (m_backends.contains(name))
	{
		return m_backends[name];
	}

	if (name.isEmpty())
	{
		const QString defaultName = SettingsManager::getValue(QLatin1String("Backends/Web")).toString();

		if (m_backends.contains(defaultName))
		{
			return m_backends[defaultName];
		}

		return m_backends.value(m_backends.keys().first(), NULL);
	}

	return NULL;
}

}
