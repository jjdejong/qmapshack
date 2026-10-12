/**********************************************************************************************
    Copyright (C) 2021 Oliver Eichler <oliver.eichler@gmx.de>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

**********************************************************************************************/

#ifndef CDEVICEGARMINMTP_H
#define CDEVICEGARMINMTP_H

#include <QPointer>

#include "device/IDevice.h"

class QDBusObjectPath;
class IDeviceAccess;
class GVFSMount;
class CThread;
class QFile;

class CDeviceGarminMtp : public QObject, public IDevice {
  Q_OBJECT
 public:
  CDeviceGarminMtp(IDeviceAccess* access, const QString& key, QTreeWidget* parent);

#ifdef HAVE_DBUS
  CDeviceGarminMtp(const QDBusObjectPath& objectPathStorage, const QString& key, QTreeWidget* parent);

  CDeviceGarminMtp(const GVFSMount& mount, const QString& storagePath, const QString& key, QTreeWidget* parent);
#endif

  virtual ~CDeviceGarminMtp();

  bool removeFromDevice(const QString& filename);

  QString getInfo(quint32) const override;
  bool supportsAutoSync() const override;
  void cancelLoading();

 protected:
  void insertCopyOfProject(IGisProject* project) override;

 private:
  void setup();
  void createProjectsFromFiles(QString subdirectory, QString extension, quint32& count, const quint32 total);
  QString createFileName(IGisProject* project, const QString& path, const QString& suffix) const;
  QString simplifiedName(IGisProject* project) const;
  void reorderProjects(IGisProject* project);
  void loadProject(QFile& file, const QString& filename, const QString& extension);
  void startAsyncLoading();
  void loadNextAsyncFile();

  IDeviceAccess* device;

  QString id;
  QString partno;
  QString description = "Garmin";
  QString softwareVersion;
  QString pathGpx;
  QString pathPictures;
  QString pathSpoilers;
  QString pathActivities;
  QString pathCourses;
  QString pathLocations;
  QString pathAdventures;  // no default
  QString pathTcx;         // no default

  QPointer<CThread> threadLoadData;
  QList<QPair<QString, QString>> pendingFiles;
  quint32 asyncTotal = 0;
  quint32 asyncCount = 0;
  bool loadingCancelled = false;
};

#endif  // CDEVICEGARMINMTP_H
