#ifndef SERIALLOGDATA_H
#define SERIALLOGDATA_H

#include <QObject>
#include <QSerialPort>

#include "logdata.h"
#include "settingsdialog.h"

class QSerialPort;

class SerialLogData : public ILogData {
  Q_OBJECT
private:
  std::vector<std::pair<QDateTime, QString>> m_lines;
  QSerialPort m_serialPort;
  SerialPortSettings m_serialPortSettings;
  int m_maxLineLength;
public:
  // Creates an empty LogData
  SerialLogData(SerialPortSettings* settings);
  // Destroy an object
  ~SerialLogData();

  virtual void attachFile( const QString& fileName );
  virtual void write(QString str) override;
  virtual bool isWritable() const override;
  virtual SerialPortSettings * GetIoSettings() override;

  // AbstractLogData interface
protected:
  QString doGetLineString(qint64 line) const override;
  QString doGetExpandedLineString(qint64 line) const override;
  QStringList doGetLines(qint64 first_line, int number) const override;
  QStringList doGetExpandedLines(qint64 first_line, int number) const override;
  // Get the length of the longest line
  qint64 doGetNbLine() const override;
  // Get the total number of lines
  int doGetMaxLength() const override;
  int doGetLineLength(qint64 line) const override;
  void doSetDisplayEncoding(Encoding encoding) override;
  void doSetMultibyteEncodingOffsets(int before_cr, int after_cr) override;

  virtual QDateTime getLastModifiedDate() const;

signals:
  // Sent during the 'attach' process to signal progress
  // percent being the percentage of completion.
  void loadingProgressed( int percent );
  // Signal the client the file is fully loaded and available.
  void loadingFinished( LoadingStatus status );
  // Sent when the file on disk has changed, will be followed
  // by loadingProgressed if needed and then a loadingFinished.
  void fileChanged( LogData::MonitoredFileStatus status );

private slots:
  void readDataSlot();
};

#endif // SERIALLOGDATA_H