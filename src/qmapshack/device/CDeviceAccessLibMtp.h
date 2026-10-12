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

#ifndef CDEVICEACCESSLIBMTP_H
#define CDEVICEACCESSLIBMTP_H

#include <QCoreApplication>
#include <QList>
#include <QThread>
#include <atomic>
#include <memory>

#include "device/IDeviceAccess.h"

class CDeviceAccessLibMtp : public IDeviceAccess {
  Q_DECLARE_TR_FUNCTIONS(CDeviceAccessLibMtp)
 public:
  struct DeviceInfo {
    quint16 vendor;
    quint16 product;
    quint32 flags;
    quint32 bus;
    quint8 number;
    QString key;
  };

  static QList<DeviceInfo> detectDevices();

  CDeviceAccessLibMtp(const DeviceInfo& info, QObject* parent = nullptr);
  ~CDeviceAccessLibMtp() override;

  bool foundValidStoragePath() const override;
  QPixmap getIcon(const QString& iconPath) override;
  QString decription() override;
  bool readFileFromStorage(const QString& path, QFile& file) override;
  bool sendFileToStorage(const QString& path, QFile& file) override;
  bool removeFileFromStorage(const QString& path) override;
  QStringList listFilesOnStorage(const QString& path) override;
  QStringList listDirsOnStorage(const QString& path) override;
  bool supportsAsyncReads() const override { return true; }
  bool supportsAutoSync() const override { return false; }
  bool requiresUploadConfirmation() const override { return true; }
  QString lastError() const override { return uploadError; }
  void cancelPendingReads() override;
  void readFileFromStorageAsync(const QString& path, QObject* receiver,
                                std::function<void(const QByteArray&)> callback) override;

 private:
  struct Session;
  QStringList listEntries(const QString& path, bool folders);

  QThread thread;
  QObject* worker;
  std::unique_ptr<Session> session;
  std::atomic_bool cancelled{false};
  QString uploadError;
};

#endif
