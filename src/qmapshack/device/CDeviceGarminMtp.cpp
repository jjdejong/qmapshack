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

#include "device/CDeviceGarminMtp.h"

#ifdef HAVE_DBUS
#include "device/CDeviceAccessGvfsMtp.h"
#include "device/CDeviceAccessKMtp.h"
#endif
#include <QMessageBox>
#include <exception>

#include "CMainWindow.h"
#include "device/CDeviceGarminArchiveMtp.h"
#include "device/IDeviceAccess.h"
#include "gis/CGisListWks.h"
#include "gis/fit2/CFit2Project.h"
#include "gis/gpx/CGpxProject.h"
#include "helpers/CThread.h"
#include "misc.h"

CDeviceGarminMtp::CDeviceGarminMtp(IDeviceAccess* access, const QString& key, QTreeWidget* parent)
    : QObject(parent), IDevice("", eTypeGarminMtp, key, parent), device(access) {
  device->setParent(this);
  setup();
}

#ifdef HAVE_DBUS
CDeviceGarminMtp::CDeviceGarminMtp(const GVFSMount& mount, const QString& storagePath, const QString& key,
                                   QTreeWidget* parent)
    : CDeviceGarminMtp(new CDeviceAccessGvfsMtp(mount, storagePath, nullptr), key, parent) {}

CDeviceGarminMtp::CDeviceGarminMtp(const QDBusObjectPath& objectPathStorage, const QString& key, QTreeWidget* parent)
    : CDeviceGarminMtp(new CDeviceAccessKMtp(objectPathStorage, nullptr), key, parent) {}
#endif

CDeviceGarminMtp::~CDeviceGarminMtp() { cancelLoading(); }

bool CDeviceGarminMtp::supportsAutoSync() const { return device->supportsAutoSync(); }

void CDeviceGarminMtp::cancelLoading() {
  loadingCancelled = true;
  device->cancelPendingReads();
  if (threadLoadData != nullptr) {
    threadLoadData->cancel();
  }
}

QString CDeviceGarminMtp::getInfo(quint32) const {
  // update the tool tip with information from the device xml
  return QString("%1 (%2, V%3)").arg(description, partno, softwareVersion);
}

void CDeviceGarminMtp::setup() {
  if (!device->foundValidStoragePath()) {
    return;
  }

  QPixmap pixmap;
  const QStringList& files = device->listFilesOnStorage("");
  if (files.contains("Garmintriangletm.ico")) {
    pixmap = device->getIcon("Garmintriangletm.ico");
  } else if (files.contains("Garmintriangletm.icon")) {
    pixmap = device->getIcon("Garmintriangletm.icon");
  }

  if (!pixmap.isNull()) {
    icon = pixmap;
  }

  // Try to read detailed information from GarminDevice.xml
  QTemporaryFile garminDeviceXmlFile;
  if (device->readFileFromStorage("GarminDevice.xml", garminDeviceXmlFile)) {
    QDomDocument dom;
    openFileCheckSuccess(QIODevice::ReadWrite, garminDeviceXmlFile);
    const QDomDocument::ParseResult& result = dom.setContent(&garminDeviceXmlFile);
    if (!result) {
      qDebug() << QString("Failed to read: %1\nline %2, column %3:\n %4")
                      .arg(garminDeviceXmlFile.fileName())
                      .arg(result.errorLine)
                      .arg(result.errorColumn)
                      .arg(result.errorMessage);
      return;
    }

    const QDomElement& xmlDevice = dom.documentElement();
    const QDomNode& xmlModel = xmlDevice.namedItem("Model");
    id = xmlDevice.namedItem("Id").toElement().text().trimmed();
    description = xmlModel.namedItem("Description").toElement().text().trimmed();
    partno = xmlModel.namedItem("PartNumber").toElement().text().trimmed();

    softwareVersion = xmlModel.namedItem("SoftwareVersion").toElement().text().trimmed();

    const QDomNode& xmlMassStorageMode = xmlDevice.namedItem("MassStorageMode");
    const QDomNodeList& xmlDataTypes = xmlMassStorageMode.toElement().elementsByTagName("DataType");

    const int N = xmlDataTypes.count();
    for (int n = 0; n < N; n++) {
      const QDomNode& xmlDataType = xmlDataTypes.item(n);
      const QDomNode& xmlName = xmlDataType.namedItem("Name");
      const QDomNode& xmlFile = xmlDataType.namedItem("File");
      const QDomNode& xmlLocation = xmlFile.namedItem("Location");
      const QDomNode& xmlPath = xmlLocation.namedItem("Path");

      QString name = xmlName.toElement().text().trimmed();

      if (name == "GPSData") {
        pathGpx = xmlPath.toElement().text().trimmed().replace("Garmin/", "", Qt::CaseInsensitive);
      } else if (name == "GeotaggedPhotos") {
        pathPictures = xmlPath.toElement().text().trimmed().replace("Garmin/", "", Qt::CaseInsensitive);
      } else if (name == "GeocachePhotos") {
        pathSpoilers = xmlPath.toElement().text().trimmed().replace("Garmin/", "", Qt::CaseInsensitive);
      } else if (name == "FIT_TYPE_4") {
        pathActivities = xmlPath.toElement().text().trimmed().replace("Garmin/", "", Qt::CaseInsensitive);
      } else if (name == "FIT_TYPE_6") {
        // courses
        pathCourses = xmlPath.toElement().text().trimmed().replace("Garmin/", "", Qt::CaseInsensitive);
      } else if (name == "FIT_TYPE_8") {
        pathLocations = xmlPath.toElement().text().trimmed().replace("Garmin/", "", Qt::CaseInsensitive);
      } else if (name == "Adventures") {
        pathAdventures = xmlPath.toElement().text().trimmed().replace("Garmin/", "", Qt::CaseInsensitive);
      } else if (name == "FitnessCourses") {
        pathTcx = xmlPath.toElement().text().trimmed().replace("Garmin/", "", Qt::CaseInsensitive);
      }
    }
  } else {
    // build paths for memory card storages that do not have a GarminDevice.xml
    pathGpx = "GPX";
    pathActivities = "Activities";
    pathCourses = "Courses";
  }

  name = QString("%1 (%2)").arg(description, device->decription());
  if (device->isReadOnly()) {
    name += tr(" [MTP, read-only]");
  } else if (device->requiresUploadConfirmation()) {
    name += tr(" [MTP, GPX upload]");
  }

  qDebug() << pathGpx;
  qDebug() << pathPictures;
  qDebug() << pathSpoilers;
  qDebug() << pathActivities;
  qDebug() << pathCourses;
  qDebug() << pathLocations;
  qDebug() << pathAdventures;
  qDebug() << pathTcx;

  if (device->supportsAsyncReads()) {
    startAsyncLoading();
    return;
  }

  threadLoadData = new CThread([this]() {
    quint32 total = 0;
    total += device->listFilesOnStorage(pathGpx).count();
    total += device->listFilesOnStorage(pathActivities).count();
    total += device->listFilesOnStorage(pathCourses).count();
    if (!pathLocations.isEmpty()) {
      total += device->listFilesOnStorage(pathCourses).count();
    }
    quint32 count = 0;

    createProjectsFromFiles(pathGpx, "gpx", count, total);
    createProjectsFromFiles(pathActivities, "fit", count, total);
    createProjectsFromFiles(pathCourses, "fit", count, total);
    if (!pathLocations.isEmpty()) {
      createProjectsFromFiles(pathLocations, "fit", count, total);
    }

    QMetaObject::invokeMethod(
        this,
        [this]() {
          QMutexLocker lock(&IGisItem::mutexItems);
          if (device->listDirsOnStorage(pathGpx).contains("Archive") &&
              !device->listFilesOnStorage(QDir(pathGpx).filePath("Archive")).isEmpty()) {
            new CDeviceGarminArchiveMtp(QDir(pathGpx).filePath("Archive"), device, this);
          };
        },
        Qt::BlockingQueuedConnection);

    setProgress(total, total);
  });

  threadLoadData->start();
}

bool CDeviceGarminMtp::removeFromDevice(const QString& filename) {
  if (device->requiresUploadConfirmation()) {
    QMessageBox::information(CMainWindow::getBestWidgetForParent(), tr("Garmin MTP"),
                             tr("Deletion and replacement are disabled for native MTP devices. "
                                "Copy a new project with a different name instead."));
    return false;
  }
  if (device->isReadOnly()) {
    return false;
  }
  qDebug() << "CDeviceGarminMtp::removeFromDevice(" << filename << ")";
  return device->removeFileFromStorage(filename);
}

void CDeviceGarminMtp::insertCopyOfProject(IGisProject* project) {
  if (device->isReadOnly()) {
    qWarning() << "Cannot send projects to this read-only MTP device";
    return;
  }
  QString filename = createFileName(project, pathGpx, ".gpx");
  const bool confirmedUpload = device->requiresUploadConfirmation();
  if (confirmedUpload) {
    if (loadingCancelled) {
      return;
    }
    QString basename = simplifiedName(project).left(120);
    if (basename.isEmpty()) {
      basename = "QMapShackTrack";
    }
    filename = QDir(pathGpx).filePath(basename + ".gpx");
    const QPointer<CDeviceGarminMtp> lifetime(this);
    const auto answer = QMessageBox::question(
        CMainWindow::getBestWidgetForParent(), tr("Upload GPX to Garmin"),
        tr("Create Garmin/%1 on %2?\n\n"
           "The file will be downloaded again to verify its contents. Existing files will never be overwritten. "
           "Deletion and automatic synchronization are disabled.\n\n"
           "After upload, disconnect the Garmin to let it import the course.")
            .arg(filename, description),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (!lifetime || answer != QMessageBox::Yes || loadingCancelled) {
      return;
    }
  }
  qDebug() << "CDeviceGarminMtp::insertCopyOfProject(" << filename << ")";

  CGpxProject* gpx = new CGpxProject(filename, project, this);
  if (!gpx->isValid()) {
    delete gpx;
    return;
  }

  QTemporaryFile file;
  openFileCheckSuccess(QIODevice::ReadWrite, file);  // saveAs will close the file
  if (!CGpxProject::saveAs(file, filename, *gpx, false)) {
    delete gpx;
    return;
  }

  if (!device->sendFileToStorage(filename, file)) {
    delete gpx;
    if (confirmedUpload) {
      QMessageBox::warning(CMainWindow::getBestWidgetForParent(), tr("Garmin upload failed"), device->lastError());
    }
    return;
  }

  // move new project to top of any sub-folder/sub-device item
  reorderProjects(gpx);
  if (confirmedUpload) {
    QMessageBox::information(CMainWindow::getBestWidgetForParent(), tr("Garmin upload verified"),
                             tr("Garmin/%1 was uploaded and its downloaded copy matches exactly.\n\n"
                                "Disconnect the Garmin and check its Courses list after processing.")
                                 .arg(filename));
  }
}

void CDeviceGarminMtp::createProjectsFromFiles(QString subdirectory, QString extension, quint32& count,
                                               const quint32 total) {
  QDir d(subdirectory);
  const QStringList& files = device->listFilesOnStorage(subdirectory);
  for (const QString& file : files) {
    if (QThread::currentThread()->isInterruptionRequested()) {
      return;
    }

    setProgress(++count, total);
    if (!file.endsWith(extension, Qt::CaseInsensitive)) {
      continue;
    }

    QMetaObject::invokeMethod(
        this,
        [this, d, file, extension]() {
          QTemporaryFile tempFile;
          if (!device->readFileFromStorage(d.filePath(file), tempFile)) {
            return;
          }
          openFileCheckSuccess(QIODevice::ReadWrite, tempFile);

          loadProject(tempFile, d.filePath(file), extension);
        },
        Qt::BlockingQueuedConnection);
  }
}

void CDeviceGarminMtp::loadProject(QFile& file, const QString& filename, const QString& extension) {
  QMutexLocker lock(&IGisItem::mutexItems);
  try {
    IGisProject* project = nullptr;
    if (extension == "gpx") {
      project = new CGpxProject(file, filename, this);
    } else if (extension == "fit") {
      project = new CFit2Project(file, filename, this);
    }
    if (project != nullptr) {
      if (!project->isValid()) {
        project->destroyLater();
      } else {
        project->setVisibility(isVisible());
      }
    }
  } catch (const QString& error) {
    qWarning() << error;
  } catch (const std::exception& error) {
    qWarning() << "Could not decode MTP project" << filename << error.what();
  }
}

void CDeviceGarminMtp::startAsyncLoading() {
  QSet<QString> visited;
  const QList<QPair<QString, QString>> paths = {
      {pathGpx, "gpx"}, {pathActivities, "fit"}, {pathCourses, "fit"}, {pathLocations, "fit"}};
  for (const auto& path : paths) {
    if (path.first.isEmpty() || visited.contains(path.first)) {
      continue;
    }
    visited.insert(path.first);
    const auto files = device->listFilesOnStorage(path.first);
    for (const QString& filename : files) {
      if (filename.endsWith("." + path.second, Qt::CaseInsensitive)) {
        pendingFiles.append({QDir(path.first).filePath(filename), path.second});
      }
    }
  }
  asyncTotal = pendingFiles.size();
  qInfo() << "Native MTP: loading" << asyncTotal << "Garmin projects";
  QTimer::singleShot(0, this, &CDeviceGarminMtp::loadNextAsyncFile);
}

void CDeviceGarminMtp::loadNextAsyncFile() {
  if (loadingCancelled) {
    return;
  }
  if (pendingFiles.isEmpty()) {
    setProgress(asyncTotal, asyncTotal);
    qInfo() << "Native MTP: finished loading" << childCount() << "Garmin projects";
    return;
  }
  const auto pending = pendingFiles.takeFirst();
  device->readFileFromStorageAsync(pending.first, this, [this, pending](const QByteArray& contents) {
    if (loadingCancelled) {
      return;
    }
    QTemporaryFile file;
    if (!contents.isEmpty() && file.open() && file.write(contents) == contents.size() && file.flush() && file.seek(0)) {
      loadProject(file, pending.first, pending.second);
    } else {
      qWarning() << "Could not load MTP project" << pending.first;
    }
    setProgress(++asyncCount, asyncTotal);
    QTimer::singleShot(0, this, &CDeviceGarminMtp::loadNextAsyncFile);
  });
}

QString CDeviceGarminMtp::createFileName(IGisProject* project, const QString& path, const QString& suffix) const {
  return QDir(path).filePath(simplifiedName(project) + suffix);
}

QString CDeviceGarminMtp::simplifiedName(IGisProject* project) const {
  static const QRegularExpression re("[^A-Za-z0-9_]");
  QString tempName = project->getName();
  return tempName.remove(re);
}

void CDeviceGarminMtp::reorderProjects(IGisProject* project) {
  // move new project to top of any sub-folder/sub-device item
  int newIdx = NOIDX;
  const int myIdx = childCount() - 1;
  for (int i = myIdx - 1; i >= 0; i--) {
    IDevice* device = dynamic_cast<IDevice*>(child(i));
    if (0 == device) {
      break;
    }

    newIdx = i;
  }

  if (newIdx != NOIDX) {
    takeChild(myIdx);
    insertChild(newIdx, project);
  }
}
