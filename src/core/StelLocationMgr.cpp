/*
 * Copyright (C) 2008 Fabien Chereau
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "StelLocationMgr.hpp"
#include "StelLocationMgr_p.hpp"

#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelFileMgr.hpp"
#include "StelUtils.hpp"
#include "StelJsonParser.hpp"
#include "StelLocaleMgr.hpp"

#include <QStringListModel>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QNetworkInterface>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QSettings>
#include <QTimeZone>

TimezoneNameMap StelLocationMgr::locationDBToIANAtranslations;

#ifdef ENABLE_GPS
#ifdef ENABLE_LIBGPS
LibGPSLookupHelper::LibGPSLookupHelper(QObject *parent)
	: GPSLookupHelper(parent), ready(false)
{
	QSettings* conf = StelApp::getInstance().getSettings();

	QString gpsdHostname=conf->value("gui/gpsd_hostname", "localhost").toString();
	QString gpsdPort=conf->value("gui/gpsd_port", DEFAULT_GPSD_PORT).toString();

	// Example almost straight from http://www.catb.org/gpsd/client-howto.html
	gps_rec = new gpsmm(gpsdHostname.toUtf8(), gpsdPort.toUtf8());
	if(gps_rec->is_open())
	{
		ready = gps_rec->stream(WATCH_ENABLE|WATCH_JSON);
	}
	if(!ready)
		qDebug()<<"libGPS lookup not ready, GPSD probably not running";
}

LibGPSLookupHelper::~LibGPSLookupHelper()
{
	delete gps_rec;
}

bool LibGPSLookupHelper::isReady()
{
	return ready;
}

void LibGPSLookupHelper::query()
{
	if(!ready)
	{
		emit queryError("GPSD helper not ready");
		return;
	}

	StelLocation loc;

	int tries=0;
	int fixmode=0;
	while (tries<10)
	{
		tries++;

		if (!gps_rec->waiting(750000)) // argument usec. wait 0.75 sec. (example had 50s)
		{
			//qDebug() << " - waiting timed out after 0.75sec.";
			continue;
		}

		struct gps_data_t* newdata;
		if ((newdata = gps_rec->read()) == NULL)
		{
			emit queryError("GPSD query: Read error.");
			return;
		}
		else
		{
// It is unclear why some data elements seem to be not filled by gps_rec.read().
//			if (newdata->status==0) // no fix?
//			{
//				// This can happen indoors.
//				qDebug() << "GPS has no fix.";
//				emit gpsResult(false);
//				return;
//			}
			if (newdata->online==0) // no device?
			{
				// This can happen when unplugging the GPS while running Stellarium,
				// or running gpsd with no GPS receiver.
				emit queryError("GPS seems offline. No fix.");
				return;
			}

			fixmode=newdata->fix.mode; // 0:not_seen, 1:no_fix, 2:2Dfix(no alt), 3:3Dfix(perfect)
			loc.longitude=newdata->fix.longitude;
			loc.latitude=newdata->fix.latitude;
			// Frequently hdop, vdop and satellite counts are NaN. Sometimes they show OK. This is minor issue.
			if (fixmode<3)
			{
				qDebug() << "GPSDfix " << fixmode << ": Location" << QString("lat %1, long %2, alt %3").arg(loc.latitude).arg(loc.longitude).arg(loc.altitude);
				qDebug() << "    Estimated HDOP " << newdata->dop.hdop << "m from " << newdata->satellites_used << "(of" << newdata->satellites_visible  << "visible) satellites";
			}
			else
			{
				loc.altitude=newdata->fix.altitude;
				qDebug() << "GPSDfix " << fixmode << ": Location" << QString("lat %1, long %2, alt %3").arg(loc.latitude).arg(loc.longitude).arg(loc.altitude);
				qDebug() << "    Estimated HDOP " << newdata->dop.hdop << "m, VDOP " << newdata->dop.vdop <<  "m from " << newdata->satellites_used << "(of" << newdata->satellites_visible  << "visible) satellites";
				break; // escape from the tries loop
			}
		}
	}

	if (fixmode <2)
	{
		emit queryError("GPSD: Could not get valid position.");
		return;
	}
	if (fixmode<3)
	{
		qDebug() << "Fix only quality " << fixmode << " after " << tries << " tries";
	}
	qDebug() << "GPSD location" << QString("lat %1, long %2, alt %3").arg(loc.latitude).arg(loc.longitude).arg(loc.altitude);

	loc.bortleScaleIndex=StelLocation::DEFAULT_BORTLE_SCALE_INDEX;
	// Usually you don't leave your time zone with GPS.
	loc.ianaTimeZone=StelApp::getInstance().getCore()->getCurrentTimeZone();
	loc.isUserLocation=true;
	loc.planetName="Earth";
	loc.name=QString("GPS %1%2 %3%4")
			.arg(loc.longitude<0?"W":"E").arg(floor(loc.longitude))
			.arg(loc.latitude<0?"S":"N").arg(floor(loc.latitude));
	emit queryFinished(loc);
}

#endif

NMEALookupHelper::NMEALookupHelper(QObject *parent)
	: GPSLookupHelper(parent), serial(NULL), nmea(NULL)
{
	//use RAII
	// Getting a list of ports may enable auto-detection!
	QList<QSerialPortInfo> portInfoList=QSerialPortInfo::availablePorts();

	if (portInfoList.size()==0)
	{
		qDebug() << "No connected devices found. NMEA GPS lookup failed.";
		return;
	}

	QSettings* conf = StelApp::getInstance().getSettings();

	// As long as we only have one, this is OK. Else we must do something about COM3, COM4 etc.
	QSerialPortInfo portInfo;
	if (portInfoList.size()==1)
	{
		portInfo=portInfoList.at(0);
	}
	else
	{
		#ifdef Q_OS_WIN
		QString portName=conf->value("gui/gps_interface", "COM3").toString();
		#else
		QString portName=conf->value("gui/gps_interface", "ttyUSB0").toString();
		#endif
		bool portFound=false;
		for (int i=0; i<portInfoList.size(); ++i)
		{
			QSerialPortInfo pi=portInfoList.at(i);
			qDebug() << "Serial port list. Make sure you are using the right configuration.";
			qDebug() << "Port: " << pi.portName();
			qDebug() << "  SystemLocation:" << pi.systemLocation();
			qDebug() << "  Description:"    << pi.description();
			qDebug() << "  Manufacturer:"   << pi.manufacturer();
			qDebug() << "  VendorID:"       << pi.vendorIdentifier();
			qDebug() << "  ProductID:"      << pi.productIdentifier();
			qDebug() << "  SerialNumber:"   << pi.serialNumber();
			qDebug() << "  Busy:"           << pi.isBusy();
			qDebug() << "  Valid:"          << pi.isValid();
			qDebug() << "  Null:"           << pi.isNull();
			if (pi.portName()==portName)
			{
				portInfo=pi;
				portFound=true;
			}
		}
		if (!portFound)
		{
			qDebug() << "Configured port" << portName << "not found. No GPS query.";
			return;
		}
	}

	// NMEA-0183 specifies device sends at 4800bps, 8N1. Some devices however send at 9600, allow this.
	// baudrate is configurable via config
	qint32 baudrate=conf->value("gui/gps_baudrate", 4800).toInt();

	nmea=new QNmeaPositionInfoSource(QNmeaPositionInfoSource::RealTimeMode,this);
	serial = new QSerialPort(portInfo, nmea);
	serial->setBaudRate(baudrate);
	serial->setDataBits(QSerialPort::Data8);
	serial->setParity(QSerialPort::NoParity);
	serial->setStopBits(QSerialPort::OneStop);
	serial->setFlowControl(QSerialPort::NoFlowControl);
	//serial->open(QIODevice::ReadOnly); // automatic by setDevice below.

	nmea->setDevice(serial);
	// TODO Find out what happens if some other serial device is connected? Just timeout/error?

	qDebug() << "Query GPS NMEA device at port " << serial->portName();
	connect(nmea, SIGNAL(error(QGeoPositionInfoSource::Error)), this, SLOT(nmeaError(QGeoPositionInfoSource::Error)));
	connect(nmea, SIGNAL(positionUpdated(const QGeoPositionInfo)),this,SLOT(nmeaUpdated(const QGeoPositionInfo)));
	connect(nmea, SIGNAL(updateTimeout()),this,SLOT(nmeaTimeout()));
}

void NMEALookupHelper::query()
{
	if(isReady())
	{
		//kick off a update request
		nmea->requestUpdate(3000);
	}
	else
		emit queryError("NMEA helper not ready");
}

void NMEALookupHelper::nmeaUpdated(const QGeoPositionInfo &update)
{
	qDebug() << "NMEA updated";

	QGeoCoordinate coord=update.coordinate();
	QDateTime timestamp=update.timestamp();

	qDebug() << " - time: " << timestamp.toString();
	qDebug() << " - location: Long=" << coord.longitude() << " Lat=" << coord.latitude() << " Alt=" << coord.altitude();

	if (update.isValid())
	{
		StelCore *core=StelApp::getInstance().getCore();
		StelLocation loc;
		loc.longitude=coord.longitude();
		loc.latitude=coord.latitude();
		// 2D fix may have only long/lat, invalid altitude.
		loc.altitude=( qIsNaN(coord.altitude()) ? 0 : (int) floor(coord.altitude()));
		loc.bortleScaleIndex=StelLocation::DEFAULT_BORTLE_SCALE_INDEX;
		// Usually you don't leave your time zone with GPS.
		loc.ianaTimeZone=core->getCurrentTimeZone();
		loc.isUserLocation=true;
		loc.planetName="Earth";
		loc.name=QString("GPS %1%2 %3%4")
				.arg(loc.longitude<0?"W":"E").arg(floor(loc.longitude))
				.arg(loc.latitude<0?"S":"N").arg(floor(loc.latitude));

		emit queryFinished(loc);
	}
	else
	{
		emit queryError("NMEA update: invalid package");
	}
}

void NMEALookupHelper::nmeaError(QGeoPositionInfoSource::Error error)
{
	emit queryError(QString("NMEA general error: %1").arg(error));
}

void NMEALookupHelper::nmeaTimeout()
{
	emit queryError("NMEA timeout");
}
#endif

StelLocationMgr::StelLocationMgr()
	: nmeaHelper(NULL), libGpsHelper(NULL)
{
	// initialize the static QMap first if necessary.
	if (locationDBToIANAtranslations.count()==0)
	{
		// reported in SF forum on 2017-03-27
		locationDBToIANAtranslations.insert("Europe/Minsk",     "UTC+03:00");
		locationDBToIANAtranslations.insert("Europe/Samara",    "UTC+04:00");
		locationDBToIANAtranslations.insert("America/Cancun",   "UTC-05:00");
		locationDBToIANAtranslations.insert("Asia/Kamchatka",   "UTC+12:00");
		// Missing on Qt5.7/Win10 as of 2017-03-18.
		locationDBToIANAtranslations.insert("Europe/Astrakhan", "UTC+04:00");
		locationDBToIANAtranslations.insert("Europe/Ulyanovsk", "UTC+04:00");
		locationDBToIANAtranslations.insert("Europe/Kirov",     "UTC+03:00");
		locationDBToIANAtranslations.insert("Asia/Hebron",      "Asia/Jerusalem");
		locationDBToIANAtranslations.insert("Asia/Gaza",        "Asia/Jerusalem"); // or use UTC+2:00? (political issue...)
		locationDBToIANAtranslations.insert("Asia/Kolkata",     "Asia/Calcutta");
		locationDBToIANAtranslations.insert("Asia/Kathmandu",   "Asia/Katmandu");
		locationDBToIANAtranslations.insert("Asia/Tomsk",       "Asia/Novosibirsk");
		locationDBToIANAtranslations.insert("Asia/Barnaul",     "UTC+07:00");
		locationDBToIANAtranslations.insert("Asia/Ho_Chi_Minh", "Asia/Saigon");
		locationDBToIANAtranslations.insert("Asia/Hovd",        "UTC+07:00");
		locationDBToIANAtranslations.insert("America/Argentina/Buenos_Aires", "America/Buenos_Aires");
		locationDBToIANAtranslations.insert("America/Argentina/Jujuy",        "America/Jujuy");
		locationDBToIANAtranslations.insert("America/Argentina/Mendoza",      "America/Mendoza");
		locationDBToIANAtranslations.insert("America/Argentina/Catamarca",    "America/Catamarca");
		locationDBToIANAtranslations.insert("America/Argentina/Cordoba",      "America/Cordoba");
		locationDBToIANAtranslations.insert("America/Indiana/Indianapolis",   "America/Indianapolis");
		locationDBToIANAtranslations.insert("America/Kentucky/Louisville",    "America/Louisville");
		locationDBToIANAtranslations.insert("America/Miquelon",               "UTC-03:00");  // Small Canadian island.
		locationDBToIANAtranslations.insert("Africa/Asmara",     "Africa/Asmera");
		locationDBToIANAtranslations.insert("Atlantic/Faroe",    "Atlantic/Faeroe");
		locationDBToIANAtranslations.insert("Pacific/Pohnpei",   "Pacific/Ponape");
		locationDBToIANAtranslations.insert("Pacific/Norfolk",   "UTC+11:00");
		locationDBToIANAtranslations.insert("Pacific/Pitcairn",  "UTC-08:00");
		// Missing on Qt5.5.1/Ubuntu 16.04.1 LTE as of 2017-03-18
		locationDBToIANAtranslations.insert("Asia/Rangoon",      "Asia/Yangon"); // UTC+6:30 Missing on Ubuntu/Qt5.5.1.
		locationDBToIANAtranslations.insert( "", "UTC");
		// N.B. Further missing TZ names will be printed out in the log.txt. Resolve these by adding into this list.
		// TODO later: create a text file in user data directory, and auto-update it weekly.
	}

	QSettings* conf = StelApp::getInstance().getSettings();

	// The line below allows to re-generate the location file, you still need to gunzip it manually afterward.
	if (conf->value("devel/convert_locations_list", false).toBool())
		generateBinaryLocationFile("data/base_locations.txt", false, "data/base_locations.bin");

	locations = loadCitiesBin("data/base_locations.bin.gz");
	locations.unite(loadCities("data/user_locations.txt", true));
	
	// Init to Paris France because it's the center of the world.
	lastResortLocation = locationForString(conf->value("init_location/last_location", "Paris, France").toString());
}

StelLocationMgr::~StelLocationMgr()
{

}

StelLocationMgr::StelLocationMgr(const LocationList &locations)
	: nmeaHelper(NULL), libGpsHelper(NULL)
{
	setLocations(locations);

	QSettings* conf = StelApp::getInstance().getSettings();
	// Init to Paris France because it's the center of the world.
	lastResortLocation = locationForString(conf->value("init_location/last_location", "Paris, France").toString());
}

void StelLocationMgr::setLocations(const LocationList &locations)
{
	for(LocationList::const_iterator it = locations.constBegin();it!=locations.constEnd();++it)
	{
		this->locations.insert(it->getID(),*it);
	}

	emit locationListChanged();
}

void StelLocationMgr::generateBinaryLocationFile(const QString& fileName, bool isUserLocation, const QString& binFilePath) const
{
	qWarning() << "Generating a locations list...";
	const QMap<QString, StelLocation>& cities = loadCities(fileName, isUserLocation);
	QFile binfile(StelFileMgr::findFile(binFilePath));
	if(binfile.open(QIODevice::WriteOnly))
	{
		QDataStream out(&binfile);
		out.setVersion(QDataStream::Qt_5_2);
		out << cities;
		binfile.close();
	}
}

LocationMap StelLocationMgr::loadCitiesBin(const QString& fileName)
{
	QMap<QString, StelLocation> res;
	QString cityDataPath = StelFileMgr::findFile(fileName);
	if (cityDataPath.isEmpty())
		return res;

	QFile sourcefile(cityDataPath);
	if (!sourcefile.open(QIODevice::ReadOnly))
	{
		qWarning() << "ERROR: Could not open location data file: " << QDir::toNativeSeparators(cityDataPath);
		return res;
	}

	if (fileName.endsWith(".gz"))
	{
		QDataStream in(StelUtils::uncompress(sourcefile.readAll()));
		in.setVersion(QDataStream::Qt_5_2);
		in >> res;
	}
	else
	{
		QDataStream in(&sourcefile);
		in.setVersion(QDataStream::Qt_5_2);
		in >> res;
	}
	// Now res has all location data. However, some timezone names are not available in various versions of Qt.
	// Sanity checks: It seems we must translate timezone names. Quite a number on Windows, but also still some on Linux.
	QList<QByteArray> availableTimeZoneList=QTimeZone::availableTimeZoneIds();
	QStringList unknownTZlist;
	QMap<QString, StelLocation>::iterator i=res.begin();
	while (i!=res.end())
	{
		StelLocation loc=i.value();
		if ((loc.ianaTimeZone!="LMST") &&  (loc.ianaTimeZone!="LTST") && ( ! availableTimeZoneList.contains(loc.ianaTimeZone.toUtf8())) )
		{
			// TZ name which is currently unknown to Qt detected. See if we can translate it, if not: complain to qDebug().
			QString fixTZname=sanitizeTimezoneStringFromLocationDB(loc.ianaTimeZone);
			if (availableTimeZoneList.contains(fixTZname.toUtf8()))
			{
				loc.ianaTimeZone=fixTZname;
				i.value() = loc;
			}
			else
			{
				qDebug() << "StelLocationMgr::loadCitiesBin(): TimeZone for " << loc.name <<  " not found: " << loc.ianaTimeZone;
				unknownTZlist.append(loc.ianaTimeZone);
			}
		}
		++i;
	}
	if (unknownTZlist.length()>0)
	{
		unknownTZlist.removeDuplicates();
		qDebug() << "StelLocationMgr::loadCitiesBin(): Summary of unknown TimeZones:";
		QStringList::const_iterator t=unknownTZlist.begin();
		while (t!=unknownTZlist.end())
		{
			qDebug() << *t;
			++t;
		}
		qDebug() << "Please report these timezone names (this logfile) to the Stellarium developers.";
		// Note to developers: Fill those names and replacements to the map above.
	}

	return res;
}

// Done in the following: TZ name sanitizing also for text file!
LocationMap StelLocationMgr::loadCities(const QString& fileName, bool isUserLocation)
{
	// Load the cities from data file
	QMap<QString, StelLocation> locations;
	QString cityDataPath = StelFileMgr::findFile(fileName);
	if (cityDataPath.isEmpty())
	{
		// Note it is quite normal not to have a user locations file (e.g. first run)
		if (!isUserLocation)
			qWarning() << "WARNING: Failed to locate location data file: " << QDir::toNativeSeparators(fileName);
		return locations;
	}

	QFile sourcefile(cityDataPath);
	if (!sourcefile.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		qWarning() << "ERROR: Could not open location data file: " << QDir::toNativeSeparators(cityDataPath);
		return locations;
	}

	// Read the data serialized from the file.
	// Code below borrowed from Marble (http://edu.kde.org/marble/)
	QTextStream sourcestream(&sourcefile);
	sourcestream.setCodec("UTF-8");
	StelLocation loc;
	while (!sourcestream.atEnd())
	{
		const QString& rawline=sourcestream.readLine();
		if (rawline.isEmpty() || rawline.startsWith('#') || (rawline.split("\t").count() < 8))
			continue;
		loc = StelLocation::createFromLine(rawline);
		loc.isUserLocation = isUserLocation;
		const QString& locId = loc.getID();

		if (locations.contains(locId))
		{
			// Add the state in the name of the existing one and the new one to differentiate
			StelLocation loc2 = locations[locId];
			if (!loc2.state.isEmpty())
				loc2.name += " ("+loc2.state+")";
			// remove and re-add the fixed version
			locations.remove(locId);
			locations.insert(loc2.getID(), loc2);

			if (!loc.state.isEmpty())
				loc.name += " ("+loc.state+")";
			locations.insert(loc.getID(), loc);
		}
		else
		{
			locations.insert(locId, loc);
		}
	}
	sourcefile.close();
	return locations;
}

static float parseAngle(const QString& s, bool* ok)
{
	float ret;
	// First try normal decimal value.
	ret = s.toFloat(ok);
	if (*ok) return ret;
	// Try GPS coordinate like +121°33'38.28"
	QRegExp reg("([+-]?[\\d.]+)°(?:([\\d.]+)')?(?:([\\d.]+)\")?");
	if (reg.exactMatch(s))
	{
		float deg = reg.capturedTexts()[1].toFloat(ok);
		if (!*ok) return 0;
		float min = reg.capturedTexts()[2].isEmpty()? 0 : reg.capturedTexts()[2].toFloat(ok);
		if (!*ok) return 0;
		float sec = reg.capturedTexts()[3].isEmpty()? 0 : reg.capturedTexts()[3].toFloat(ok);
		if (!*ok) return 0;
		return deg + min / 60 + sec / 3600;
	}
	return 0;
}

const StelLocation StelLocationMgr::locationForString(const QString& s) const
{
	QMap<QString, StelLocation>::const_iterator iter = locations.find(s);
	if (iter!=locations.end())
	{
		return iter.value();
	}
	StelLocation ret;
	// Maybe it is a coordinate set ? (e.g. GPS 25.107363,121.558807 )
	QRegExp reg("(?:(.+)\\s+)?(.+),(.+)");
	if (reg.exactMatch(s))
	{
		bool ok;
		// We have a set of coordinates
		ret.latitude = parseAngle(reg.capturedTexts()[2].trimmed(), &ok);
		if (!ok) ret.role = '!';
		ret.longitude = parseAngle(reg.capturedTexts()[3].trimmed(), &ok);
		if (!ok) ret.role = '!';
		ret.name = reg.capturedTexts()[1].trimmed();
		ret.planetName = "Earth";
		return ret;
	}
	ret.role = '!';
	return ret;
}

const StelLocation StelLocationMgr::locationFromCLI() const
{
	StelLocation ret;
	QSettings* conf = StelApp::getInstance().getSettings();
	bool ok;
	conf->beginGroup("location_run_once");
	ret.latitude = parseAngle(StelUtils::radToDmsStr(conf->value("latitude").toFloat(), true), &ok);
	if (!ok) ret.role = '!';
	ret.longitude = parseAngle(StelUtils::radToDmsStr(conf->value("longitude").toFloat(), true), &ok);
	if (!ok) ret.role = '!';
	ret.altitude = conf->value("altitude", 0).toInt(&ok);
	ret.planetName = conf->value("home_planet", "Earth").toString();
	ret.landscapeKey = conf->value("landscape_name", "guereins").toString();
	conf->endGroup();
	conf->remove("location_run_once");
	return ret;
}

// Get whether a location can be permanently added to the list of user locations
bool StelLocationMgr::canSaveUserLocation(const StelLocation& loc) const
{
	return loc.isValid() && locations.find(loc.getID())==locations.end();
}

// Add permanently a location to the list of user locations
bool StelLocationMgr::saveUserLocation(const StelLocation& loc)
{
	if (!canSaveUserLocation(loc))
		return false;

	// Add in the program
	locations[loc.getID()]=loc;

	//emit before saving the list
	emit locationListChanged();

	// Append to the user location file
	QString cityDataPath = StelFileMgr::findFile("data/user_locations.txt", StelFileMgr::Flags(StelFileMgr::Writable|StelFileMgr::File));
	if (cityDataPath.isEmpty())
	{
		if (!StelFileMgr::exists(StelFileMgr::getUserDir()+"/data"))
		{
			if (!StelFileMgr::mkDir(StelFileMgr::getUserDir()+"/data"))
			{
				qWarning() << "ERROR - cannot create non-existent data directory" << QDir::toNativeSeparators(StelFileMgr::getUserDir()+"/data");
				qWarning() << "Location cannot be saved";
				return false;
			}
		}

		cityDataPath = StelFileMgr::getUserDir()+"/data/user_locations.txt";
		qWarning() << "Will create a new user location file: " << QDir::toNativeSeparators(cityDataPath);
	}

	QFile sourcefile(cityDataPath);
	if (!sourcefile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append))
	{
		qWarning() << "ERROR: Could not open location data file: " << QDir::toNativeSeparators(cityDataPath);
		return false;
	}

	QTextStream outstream(&sourcefile);
	outstream.setCodec("UTF-8");
	outstream << loc.serializeToLine() << '\n';
	sourcefile.close();

	return true;
}

// Get whether a location can be deleted from the list of user locations
// If the location comes from the base read only list, it cannot be deleted
bool StelLocationMgr::canDeleteUserLocation(const QString& id) const
{
	QMap<QString, StelLocation>::const_iterator iter=locations.find(id);

	// If it's not known at all there is a problem
	if (iter==locations.end())
		return false;

	return iter.value().isUserLocation;
}

// Delete permanently the given location from the list of user locations
// If the location comes from the base read only list, it cannot be deleted and false is returned
bool StelLocationMgr::deleteUserLocation(const QString& id)
{
	if (!canDeleteUserLocation(id))
		return false;

	locations.remove(id);

	//emit before saving the list
	emit locationListChanged();

	// Resave the whole remaining user locations file
	QString cityDataPath = StelFileMgr::findFile("data/user_locations.txt", StelFileMgr::Writable);
	if (cityDataPath.isEmpty())
	{
		if (!StelFileMgr::exists(StelFileMgr::getUserDir()+"/data"))
		{
			if (!StelFileMgr::mkDir(StelFileMgr::getUserDir()+"/data"))
			{
				qWarning() << "ERROR - cannot create non-existent data directory" << QDir::toNativeSeparators(StelFileMgr::getUserDir()+"/data");
				qWarning() << "Location cannot be saved";
				return false;
			}
		}

		cityDataPath = StelFileMgr::getUserDir()+"/data/user_locations.txt";
		qWarning() << "Will create a new user location file: " << QDir::toNativeSeparators(cityDataPath);
	}

	QFile sourcefile(cityDataPath);
	if (!sourcefile.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		qWarning() << "ERROR: Could not open location data file: " << QDir::toNativeSeparators(cityDataPath);
		return false;
	}

	QTextStream outstream(&sourcefile);
	outstream.setCodec("UTF-8");

	for (QMap<QString, StelLocation>::ConstIterator iter=locations.constBegin();iter!=locations.constEnd();++iter)
	{
		if (iter.value().isUserLocation)
		{
			outstream << iter.value().serializeToLine() << '\n';
		}
	}

	sourcefile.close();
	return true;
}

// lookup location from IP address.
void StelLocationMgr::locationFromIP()
{
	QNetworkRequest req( QUrl( QString("http://freegeoip.net/json/") ) );
	req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
	req.setRawHeader("User-Agent", StelUtils::getUserAgentString().toLatin1());
	QNetworkReply* networkReply=StelApp::getInstance().getNetworkAccessManager()->get(req);
	connect(networkReply, SIGNAL(finished()), this, SLOT(changeLocationFromNetworkLookup()));
}

#ifdef ENABLE_GPS
bool StelLocationMgr::locationFromGPS()
{
#ifdef ENABLE_LIBGPS
	if(!libGpsHelper)
	{
		libGpsHelper = new LibGPSLookupHelper(this);
		connect(libGpsHelper, SIGNAL(queryFinished(StelLocation)), this, SLOT(changeLocationFromGPSQuery(StelLocation)));
		connect(libGpsHelper, SIGNAL(queryError(QString)), this, SLOT(gpsQueryError(QString)));
	}
	if(libGpsHelper->isReady())
	{
		libGpsHelper->query();
		return true;
	}
#endif
	if(!nmeaHelper)
	{
		nmeaHelper = new NMEALookupHelper(this);
		connect(nmeaHelper, SIGNAL(queryFinished(StelLocation)), this, SLOT(changeLocationFromGPSQuery(StelLocation)));
		connect(nmeaHelper, SIGNAL(queryError(QString)), this, SLOT(gpsQueryError(QString)));
	}
	if(nmeaHelper->isReady())
	{
		nmeaHelper->query();
		return true;
	}

	emit gpsQueryFinished(false);
	return false;
}

void StelLocationMgr::changeLocationFromGPSQuery(const StelLocation &loc)
{
	StelApp::getInstance().getCore()->moveObserverTo(loc, 0.0f, 0.0f);
	emit gpsQueryFinished(true);
}

void StelLocationMgr::gpsQueryError(const QString &err)
{
	qWarning()<<err;
	emit gpsQueryFinished(false);
}
#endif

// slot that receives IP-based location data from the network.
void StelLocationMgr::changeLocationFromNetworkLookup()
{
	StelCore *core=StelApp::getInstance().getCore();
	QNetworkReply* networkReply = qobject_cast<QNetworkReply*>(sender());
	if (!networkReply)
	    return;
	if (networkReply->error() == QNetworkReply::NoError) {
		//success
		QVariantMap locMap = StelJsonParser::parse(networkReply->readAll()).toMap();
		QString ipRegion = locMap.value("region_name").toString();
		QString ipCity = locMap.value("city").toString();
		QString ipCountry = locMap.value("country_name").toString(); // NOTE: Got a short name of country
		QString ipCountryCode = locMap.value("country_code").toString();
		QString ipTimeZone = locMap.value("time_zone").toString();
		float latitude=locMap.value("latitude").toFloat();
		float longitude=locMap.value("longitude").toFloat();

		qDebug() << "Got location" << QString("%1, %2, %3 (%4, %5; %6)").arg(ipCity).arg(ipRegion).arg(ipCountry).arg(latitude).arg(longitude).arg(ipTimeZone) << "for IP" << locMap.value("ip").toString();

		StelLocation loc;
		loc.name    = (ipCity.isEmpty() ? QString("%1, %2").arg(latitude).arg(longitude) : ipCity);
		loc.state   = (ipRegion.isEmpty() ? "IPregion"  : ipRegion);
		loc.country = StelLocaleMgr::countryCodeToString(ipCountryCode.isEmpty() ? "" : ipCountryCode.toLower());
		loc.role    = QChar(0x0058); // char 'X'
		loc.population = 0;
		loc.latitude = latitude;
		loc.longitude = longitude;
		loc.altitude = 0;
		loc.bortleScaleIndex = StelLocation::DEFAULT_BORTLE_SCALE_INDEX;
		loc.ianaTimeZone = (ipTimeZone.isEmpty() ? "" : ipTimeZone);
		loc.planetName = "Earth";
		loc.landscapeKey = "";

		core->setCurrentTimeZone(ipTimeZone.isEmpty() ? "LMST" : ipTimeZone);
		core->moveObserverTo(loc, 0.0f, 0.0f);
		QSettings* conf = StelApp::getInstance().getSettings();
		conf->setValue("init_location/last_location", QString("%1,%2").arg(latitude).arg(longitude));
	}
	else
	{
		qDebug() << "Failure getting IP-based location: \n\t" <<networkReply->errorString();
		// If there is a problem, this must not change to some other location!
		//core->moveObserverTo(lastResortLocation, 0.0f, 0.0f);
	}
	networkReply->deleteLater();
}

LocationMap StelLocationMgr::pickLocationsNearby(const QString planetName, const float longitude, const float latitude, const float radiusDegrees)
{
	QMap<QString, StelLocation> results;
	QMapIterator<QString, StelLocation> iter(locations);
	while (iter.hasNext())
	{
		iter.next();
		const StelLocation *loc=&iter.value();
		if ( (loc->planetName == planetName) &&
				(StelLocation::distanceDegrees(longitude, latitude, loc->longitude, loc->latitude) <= radiusDegrees) )
		{
			results.insert(iter.key(), iter.value());
		}
	}
	return results;
}

LocationMap StelLocationMgr::pickLocationsInCountry(const QString country)
{
	QMap<QString, StelLocation> results;
	QMapIterator<QString, StelLocation> iter(locations);
	while (iter.hasNext())
	{
		iter.next();
		const StelLocation *loc=&iter.value();
		if (loc->country == country)
		{
			results.insert(iter.key(), iter.value());
		}
	}
	return results;
}

// Check timezone string and return either the same or the corresponding string that we use in the Stellarium location database.
// If timezone name starts with "UTC", always return unchanged.
// This is required to store timezone names exactly as we know them, and not mix ours and corrent-iana spelling flavour.
// In practice, reverse lookup to locationDBToIANAtranslations
QString StelLocationMgr::sanitizeTimezoneStringForLocationDB(QString tzString)
{
	if (tzString.startsWith("UTC"))
		return tzString;
	QByteArray res=locationDBToIANAtranslations.key(tzString.toUtf8(), "---");
	if ( res != "---")
		return QString(res);
	return tzString;
}

// Attempt to translate a timezone name from those used in Stellarium's location database to a name which is valid
// as ckeckable by QTimeZone::availableTimeZoneIds(). That list may be updated anytime and is known to differ
// between OSes. Some spellings may be different, or in some cases some names get simply translated to "UTC+HH:MM" style.
// The empty string gets translated to "UTC".
QString StelLocationMgr::sanitizeTimezoneStringFromLocationDB(QString dbString)
{
	if (dbString.startsWith("UTC"))
		return dbString;
	// Maybe silences a debug later:
	if (dbString=="")
		return "UTC";
	QByteArray res=locationDBToIANAtranslations.value(dbString.toUtf8(), "---");
	if ( res != "---")
		return QString(res);
	return dbString;
}
