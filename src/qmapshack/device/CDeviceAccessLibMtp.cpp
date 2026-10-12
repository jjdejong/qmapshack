/**********************************************************************************************
    Copyright (C) 2026 Jean Jacques de Jong

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

#include "device/CDeviceAccessLibMtp.h"

#include <libmtp.h>

#include <QCryptographicHash>
#include <QDebug>
#include <QFileInfo>
#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QXmlStreamReader>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
void initializeLibMtp() {
  static std::once_flag initialized;
  std::call_once(initialized, LIBMTP_Init);
}

QString cleanDevicePath(QString path) {
  path.replace('\\', '/');
  if (path.startsWith('/') || path.split('/').contains("..")) {
    return {};
  }
  path = QDir::cleanPath(path);
  return path == "." ? QString() : path;
}

QString validateUpload(const QString& path, const QByteArray& data) {
  static const QRegularExpression destination("\\ANewFiles/[A-Za-z0-9_][A-Za-z0-9_.-]{0,119}\\.gpx\\z",
                                              QRegularExpression::CaseInsensitiveOption);
  if (!destination.match(path).hasMatch()) {
    return CDeviceAccessLibMtp::tr(
        "Uploads are limited to GPX files directly inside Garmin/NewFiles. "
        "Use a filename of at most 124 ASCII letters, digits, underscores, dots or hyphens.");
  }
  if (data.isEmpty() || data.size() > 16 * 1024 * 1024) {
    return CDeviceAccessLibMtp::tr("The GPX upload must be nonempty and no larger than 16 MiB.");
  }
  QXmlStreamReader xml(data);
  QStringList elements;
  QString gpxNamespace;
  int points = 0;
  bool rootFound = false;
  while (!xml.atEnd()) {
    const auto token = xml.readNext();
    if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
      return CDeviceAccessLibMtp::tr("GPX uploads must not contain a DTD or unresolved entities.");
    }
    if (xml.isStartElement()) {
      const QString name = xml.name().toString();
      if (!rootFound) {
        rootFound = true;
        gpxNamespace = xml.namespaceUri().toString();
        const QString version = xml.attributes().value("version").toString();
        if (name != "gpx" || (version != "1.0" && version != "1.1") ||
            (!gpxNamespace.isEmpty() && gpxNamespace != "http://www.topografix.com/GPX/1/0" &&
             gpxNamespace != "http://www.topografix.com/GPX/1/1")) {
          return CDeviceAccessLibMtp::tr("The upload is not a supported GPX 1.0 or 1.1 document.");
        }
      }
      if (xml.namespaceUri() == gpxNamespace && (name == "trkpt" || name == "rtept" || name == "wpt")) {
        bool latitudeValid = false;
        bool longitudeValid = false;
        const double latitude = xml.attributes().value("lat").toDouble(&latitudeValid);
        const double longitude = xml.attributes().value("lon").toDouble(&longitudeValid);
        if (!latitudeValid || !longitudeValid || !std::isfinite(latitude) || !std::isfinite(longitude) ||
            latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
          return CDeviceAccessLibMtp::tr("The GPX contains invalid coordinates.");
        }
        if ((name == "trkpt" && elements == QStringList{"gpx", "trk", "trkseg"}) ||
            (name == "rtept" && elements == QStringList{"gpx", "rte"})) {
          ++points;
        }
      }
      elements.append(xml.namespaceUri() == gpxNamespace ? name : QString());
    } else if (xml.isEndElement() && !elements.isEmpty()) {
      elements.removeLast();
    }
  }
  if (xml.hasError() || !rootFound) {
    return CDeviceAccessLibMtp::tr("The GPX XML is malformed: %1").arg(xml.errorString());
  }
  if (points < 2) {
    return CDeviceAccessLibMtp::tr("The GPX must contain at least two valid track or route points.");
  }
  return {};
}
}  // namespace

struct CDeviceAccessLibMtp::Session {
  struct Entry {
    quint32 identifier = 0;
    QString name;
    quint64 bytes = 0;
    bool folder = false;
  };

  LIBMTP_mtpdevice_t* device = nullptr;
  quint32 storage = 0;
  quint32 root = 0;
  QString description;
  QHash<QString, QList<Entry>> listings;
  std::atomic_bool* cancelled;

  explicit Session(std::atomic_bool* cancelled) : cancelled(cancelled) {}

  QList<Entry> entries(quint32 parent, bool* success = nullptr) {
    if (success != nullptr) {
      *success = false;
    }
    QList<Entry> result;
    if (cancelled->load()) {
      return result;
    }
    LIBMTP_Clear_Errorstack(device);
    LIBMTP_file_t* file = LIBMTP_Get_Files_And_Folders(device, storage, parent);
    while (file != nullptr) {
      result.append(
          {file->item_id, QString::fromUtf8(file->filename), file->filesize, file->filetype == LIBMTP_FILETYPE_FOLDER});
      LIBMTP_file_t* next = file->next;
      file->next = nullptr;
      LIBMTP_destroy_file_t(file);
      file = next;
    }
    if (LIBMTP_Get_Errorstack(device) != nullptr || cancelled->load()) {
      qWarning() << "MTP directory listing failed";
      LIBMTP_Dump_Errorstack(device);
      return {};
    }
    if (success != nullptr) {
      *success = true;
    }
    return result;
  }

  bool open(const DeviceInfo& info) {
    LIBMTP_raw_device_t raw{};
    raw.device_entry.vendor_id = info.vendor;
    raw.device_entry.product_id = info.product;
    raw.device_entry.device_flags = info.flags;
    raw.bus_location = info.bus;
    raw.devnum = info.number;
    device = LIBMTP_Open_Raw_Device_Uncached(&raw);
    if (device == nullptr || LIBMTP_Get_Storage(device, 0) != 0) {
      return false;
    }
    for (LIBMTP_devicestorage_t* candidate = device->storage; candidate != nullptr; candidate = candidate->next) {
      storage = candidate->id;
      const auto topLevel = entries(LIBMTP_FILES_AND_FOLDERS_ROOT);
      for (const Entry& entry : topLevel) {
        if (!entry.folder || entry.name.compare("Garmin", Qt::CaseInsensitive) != 0) {
          continue;
        }
        const auto children = entries(entry.identifier);
        for (const Entry& child : children) {
          if (!child.folder && child.name.compare("GarminDevice.xml", Qt::CaseInsensitive) == 0) {
            root = entry.identifier;
            description = QString::fromUtf8(candidate->StorageDescription);
            listings.insert({}, children);
            return true;
          }
        }
      }
    }
    return false;
  }

  void close() {
    listings.clear();
    root = 0;
    if (device != nullptr) {
      LIBMTP_Release_Device(device);
      device = nullptr;
    }
  }

  Entry resolve(const QString& path) {
    if (path.isEmpty()) {
      return {root, {}, 0, true};
    }
    const QString parent = cleanDevicePath(QFileInfo(path).path());
    const QString filename = QFileInfo(path).fileName();
    const auto siblings = listing(parent);
    for (const Entry& entry : siblings) {
      if (entry.name.compare(filename, Qt::CaseInsensitive) == 0) {
        return entry;
      }
    }
    return {};
  }

  QList<Entry> listing(const QString& path) {
    if (root == 0 || cancelled->load()) {
      return {};
    }
    const QString cleaned = cleanDevicePath(path);
    if (!path.isEmpty() && cleaned.isEmpty() && path != ".") {
      return {};
    }
    const auto cached = listings.constFind(cleaned);
    if (cached != listings.constEnd()) {
      return cached.value();
    }
    const Entry folder = resolve(cleaned);
    if (!folder.folder || folder.identifier == 0) {
      return {};
    }
    bool success = false;
    const auto result = entries(folder.identifier, &success);
    if (success) {
      listings.insert(cleaned, result);
    }
    return result;
  }

  QByteArray read(const QString& path) {
    const QString cleaned = cleanDevicePath(path);
    if (cleaned.isEmpty() || root == 0 || cancelled->load()) {
      return {};
    }
    const Entry entry = resolve(cleaned);
    if (entry.identifier == 0 || entry.folder || entry.bytes > 256ULL * 1024 * 1024) {
      qWarning() << "MTP file is missing, invalid, or exceeds the 256 MiB read limit:" << path;
      return {};
    }
    QTemporaryFile file;
    if (!file.open()) {
      return {};
    }
    LIBMTP_Clear_Errorstack(device);
    const auto progress = [](uint64_t, uint64_t, const void* context) -> int {
      return static_cast<const std::atomic_bool*>(context)->load() ? 1 : 0;
    };
    if (LIBMTP_Get_File_To_File_Descriptor(device, entry.identifier, file.handle(), progress, cancelled) != 0 ||
        cancelled->load()) {
      if (!cancelled->load()) {
        qWarning() << "MTP download failed:" << path;
        LIBMTP_Dump_Errorstack(device);
      }
      return {};
    }
    if (!file.seek(0)) {
      return {};
    }
    const QByteArray data = file.readAll();
    if (quint64(data.size()) != entry.bytes) {
      qWarning() << "MTP download size mismatch:" << path;
      return {};
    }
    return data;
  }

  QString upload(const QString& path, const QByteArray& data) {
    if (root == 0 || cancelled->load()) {
      return CDeviceAccessLibMtp::tr("The Garmin MTP session is unavailable or cancelled.");
    }
    QTemporaryFile snapshot;
    if (!snapshot.open() || snapshot.write(data) != data.size() || !snapshot.flush() || !snapshot.seek(0)) {
      return CDeviceAccessLibMtp::tr("Could not prepare the GPX upload.");
    }
    bool success = false;
    const auto folders = entries(root, &success);
    if (!success) {
      return CDeviceAccessLibMtp::tr("Cannot check Garmin/NewFiles; no upload was attempted.");
    }
    Entry destination;
    for (const Entry& folder : folders) {
      if (folder.folder && folder.name.compare("NewFiles", Qt::CaseInsensitive) == 0) {
        if (destination.identifier != 0) {
          return CDeviceAccessLibMtp::tr("Garmin/NewFiles is ambiguous; no upload was attempted.");
        }
        destination = folder;
      }
    }
    if (destination.identifier == 0) {
      return CDeviceAccessLibMtp::tr("Garmin/NewFiles is missing; no directory will be created.");
    }
    const auto children = entries(destination.identifier, &success);
    if (!success) {
      return CDeviceAccessLibMtp::tr("Cannot check for existing files; no upload was attempted.");
    }
    const QString filename = QFileInfo(path).fileName();
    for (const Entry& child : children) {
      if (child.name.compare(filename, Qt::CaseInsensitive) == 0) {
        return CDeviceAccessLibMtp::tr(
                   "Garmin/NewFiles/%1 already exists. Rename the project; overwriting is disabled.")
            .arg(filename);
      }
    }
    if (cancelled->load()) {
      return CDeviceAccessLibMtp::tr("The GPX upload was cancelled before sending.");
    }
    std::unique_ptr<LIBMTP_file_t, decltype(&LIBMTP_destroy_file_t)> metadata(LIBMTP_new_file_t(),
                                                                              LIBMTP_destroy_file_t);
    if (!metadata) {
      return CDeviceAccessLibMtp::tr("Could not allocate MTP upload metadata.");
    }
    metadata->filename = strdup(filename.toUtf8().constData());
    if (metadata->filename == nullptr) {
      return CDeviceAccessLibMtp::tr("Could not allocate the MTP upload filename.");
    }
    metadata->filesize = data.size();
    metadata->filetype = LIBMTP_FILETYPE_UNKNOWN;
    metadata->parent_id = destination.identifier;
    metadata->storage_id = storage;
    LIBMTP_Clear_Errorstack(device);
    const auto progress = [](uint64_t, uint64_t, const void* context) -> int {
      return static_cast<const std::atomic_bool*>(context)->load() ? 1 : 0;
    };
    const int result =
        LIBMTP_Send_File_From_File_Descriptor(device, snapshot.handle(), metadata.get(), progress, cancelled);
    listings.clear();
    if (result != 0 || metadata->item_id == 0 || cancelled->load()) {
      LIBMTP_Dump_Errorstack(device);
      return CDeviceAccessLibMtp::tr(
                 "Upload failed or was cancelled. Garmin/NewFiles/%1 may contain an incomplete file. "
                 "Inspect it before retrying; nothing will be deleted automatically.")
          .arg(filename);
    }
    const Entry uploaded = resolve(path);
    if (uploaded.identifier != metadata->item_id || uploaded.bytes != quint64(data.size())) {
      return CDeviceAccessLibMtp::tr(
                 "The uploaded file could not be verified. Inspect Garmin/NewFiles/%1 before retrying.")
          .arg(filename);
    }
    const QByteArray expected = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    const QByteArray received = read(path);
    if (received.size() != data.size() || QCryptographicHash::hash(received, QCryptographicHash::Sha256) != expected) {
      return CDeviceAccessLibMtp::tr(
                 "GPX read-back verification failed. Inspect Garmin/NewFiles/%1 before retrying; "
                 "the uploaded file has not been deleted.")
          .arg(filename);
    }
    qInfo() << "Verified native MTP GPX upload" << path << data.size() << "bytes; SHA256" << expected.toHex();
    return {};
  }
};

QList<CDeviceAccessLibMtp::DeviceInfo> CDeviceAccessLibMtp::detectDevices() {
  initializeLibMtp();
  LIBMTP_raw_device_t* raw = nullptr;
  int count = 0;
  const auto result = LIBMTP_Detect_Raw_Devices(&raw, &count);
  std::unique_ptr<LIBMTP_raw_device_t, decltype(&std::free)> devices(raw, std::free);
  QList<DeviceInfo> detected;
  if (result != LIBMTP_ERROR_NONE) {
    return detected;
  }
  for (int index = 0; index < count; ++index) {
    const auto& candidate = raw[index];
    if (candidate.device_entry.vendor_id != 0x091e) {
      continue;
    }
    detected.append({candidate.device_entry.vendor_id, candidate.device_entry.product_id,
                     candidate.device_entry.device_flags, candidate.bus_location, candidate.devnum,
                     QString("libmtp:%1:%2:%3")
                         .arg(candidate.bus_location)
                         .arg(candidate.devnum)
                         .arg(candidate.device_entry.product_id)});
  }
  return detected;
}

CDeviceAccessLibMtp::CDeviceAccessLibMtp(const DeviceInfo& info, QObject* parent)
    : IDeviceAccess(parent), worker(new QObject), session(std::make_unique<Session>(&cancelled)) {
  initializeLibMtp();
  worker->moveToThread(&thread);
  connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
  thread.start();
  QMetaObject::invokeMethod(worker, [this, info]() { session->open(info); }, Qt::BlockingQueuedConnection);
}

CDeviceAccessLibMtp::~CDeviceAccessLibMtp() {
  cancelled.store(true);
  QMetaObject::invokeMethod(worker, [this]() { session->close(); }, Qt::BlockingQueuedConnection);
  thread.quit();
  thread.wait();
}

bool CDeviceAccessLibMtp::foundValidStoragePath() const { return session->root != 0; }

QPixmap CDeviceAccessLibMtp::getIcon(const QString& iconPath) {
  QByteArray data;
  QMetaObject::invokeMethod(
      worker, [this, iconPath, &data]() { data = session->read(iconPath); }, Qt::BlockingQueuedConnection);
  QPixmap pixmap;
  pixmap.loadFromData(data);
  return pixmap;
}

QString CDeviceAccessLibMtp::decription() { return session->description; }

bool CDeviceAccessLibMtp::readFileFromStorage(const QString& path, QFile& file) {
  QByteArray data;
  QMetaObject::invokeMethod(
      worker, [this, path, &data]() { data = session->read(path); }, Qt::BlockingQueuedConnection);
  if (data.isEmpty()) {
    return false;
  }
  if (!file.isOpen() && !file.open(QIODevice::WriteOnly)) {
    return false;
  }
  if (!file.seek(0) || !file.resize(0) || file.write(data) != data.size() || !file.flush()) {
    file.close();
    return false;
  }
  file.close();
  return true;
}

bool CDeviceAccessLibMtp::sendFileToStorage(const QString& path, QFile& file) {
  uploadError.clear();
  const bool wasOpen = file.isOpen();
  const qint64 position = wasOpen ? file.pos() : 0;
  if ((!wasOpen && !file.open(QIODevice::ReadOnly)) || !file.isReadable() || !file.seek(0)) {
    uploadError = tr("Could not open the local GPX file for reading.");
    if (!wasOpen) {
      file.close();
    }
    return false;
  }
  const QByteArray data = file.read(16 * 1024 * 1024 + 1);
  const bool readFailed = file.error() != QFile::NoError;
  if (wasOpen) {
    file.seek(position);
  } else {
    file.close();
  }
  uploadError = readFailed ? tr("Could not read the local GPX file.") : validateUpload(path, data);
  if (uploadError.isEmpty()) {
    QMetaObject::invokeMethod(
        worker, [this, path, data]() { uploadError = session->upload(path, data); }, Qt::BlockingQueuedConnection);
  }
  if (!uploadError.isEmpty()) {
    qWarning() << "Native MTP GPX upload rejected:" << uploadError;
  }
  return uploadError.isEmpty();
}

bool CDeviceAccessLibMtp::removeFileFromStorage(const QString&) {
  qWarning() << "Native MTP deletion is disabled";
  return false;
}

QStringList CDeviceAccessLibMtp::listEntries(const QString& path, bool folders) {
  QStringList names;
  QMetaObject::invokeMethod(
      worker,
      [this, path, folders, &names]() {
        const auto entries = session->listing(path);
        for (const auto& entry : entries) {
          if (entry.folder == folders) {
            names.append(entry.name);
          }
        }
      },
      Qt::BlockingQueuedConnection);
  names.sort(Qt::CaseInsensitive);
  return names;
}

QStringList CDeviceAccessLibMtp::listFilesOnStorage(const QString& path) { return listEntries(path, false); }

QStringList CDeviceAccessLibMtp::listDirsOnStorage(const QString& path) { return listEntries(path, true); }

void CDeviceAccessLibMtp::cancelPendingReads() { cancelled.store(true); }

void CDeviceAccessLibMtp::readFileFromStorageAsync(const QString& path, QObject* receiver,
                                                   std::function<void(const QByteArray&)> callback) {
  const QPointer<QObject> target(receiver);
  QMetaObject::invokeMethod(
      worker,
      [this, path, target, callback = std::move(callback)]() {
        const QByteArray data = session->read(path);
        QMetaObject::invokeMethod(
            this,
            [this, target, callback, data]() {
              if (target && !cancelled.load()) {
                callback(data);
              }
            },
            Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}
