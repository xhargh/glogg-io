/*
 * Copyright (C) 2009, 2010, 2013, 2014, 2015 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

// This file implements LogData, the content of a log file.

#include <iostream>

#include <cassert>

#include <QFileInfo>

#include "log.h"

#include "logdata.h"
#include "logfiltereddata.h"

// Implementation of the 'start' functions for each operation

void LogData::AttachOperation::doStart(
        LogDataWorkerThread& workerThread ) const
{
    LOG(logDEBUG) << "Attaching " << filename_.toStdString();
    workerThread.attachFile( filename_ );
    workerThread.indexAll();
}

void LogData::FullIndexOperation::doStart(
        LogDataWorkerThread& workerThread ) const
{
    LOG(logDEBUG) << "Reindexing (full)";
    workerThread.indexAll(forcedEncoding_);
}

void LogData::PartialIndexOperation::doStart(
        LogDataWorkerThread& workerThread ) const
{
    LOG(logDEBUG) << "Reindexing (partial)";
    workerThread.indexAdditionalLines();
}


// Constructs an empty log file.
// It must be displayed without error.
LogData::LogData() : AbstractLogData(), indexing_data_(),
    fileMutex_(), workerThread_( &indexing_data_ )
{
    // Start with an "empty" log
    attached_file_ = nullptr;
    currentOperation_ = nullptr;
    nextOperation_    = nullptr;

    codec_ = QTextCodec::codecForName( "ISO-8859-1" );

    // Initialise the file watcher
    connect( &FileWatcher::getFileWatcher(), &FileWatcher::fileChanged,
            this, &LogData::fileChangedOnDisk, Qt::QueuedConnection );

    // Forward the update signal
    connect( &workerThread_, &LogDataWorkerThread::indexingProgressed,
            this, &LogData::loadingProgressed );
    connect( &workerThread_, &LogDataWorkerThread::indexingFinished,
            this, &LogData::indexingFinished );

    // Starts the worker thread
    workerThread_.start();
}

LogData::~LogData()
{
    // Remove the current file from the watch list
    if ( attached_file_ )
        FileWatcher::getFileWatcher().removeFile( attached_file_->fileName() );

    // FIXME
    // workerThread_.stop();
}

//
// Public functions
//

void LogData::attachFile( const QString& fileName )
{
    LOG(logDEBUG) << "LogData::attachFile " << fileName.toStdString();

    if ( attached_file_ ) {
        // We cannot reattach
        throw CantReattachErr();
    }

    attached_file_.reset( new QFile( fileName ) );
    attached_file_->open( QIODevice::ReadOnly );

    std::shared_ptr<const LogDataOperation> operation( new AttachOperation( fileName ) );
    enqueueOperation( std::move( operation ) );
}

void LogData::interruptLoading()
{
    workerThread_.interrupt();
}

qint64 LogData::getFileSize() const
{
    return indexing_data_.getSize();
}

QDateTime LogData::getLastModifiedDate() const
{
    return lastModifiedDate_;
}

// Return an initialised LogFilteredData. The search is not started.
LogFilteredData* LogData::getNewFilteredData() const
{
    auto* newFilteredData = new LogFilteredData( this );

    return newFilteredData;
}

void LogData::reload(QTextCodec* forcedEncoding)
{
    workerThread_.interrupt();

    // Re-open the file, useful in case the file has been moved
    reOpenFile();

    enqueueOperation( std::make_shared<FullIndexOperation>(forcedEncoding) );
}

//
// Private functions
//

// Add an operation to the queue and perform it immediately if
// there is none ongoing.
void LogData::enqueueOperation( std::shared_ptr<const LogDataOperation> new_operation )
{
    if ( currentOperation_ == nullptr )
    {
        // We do it immediately
        currentOperation_ =  new_operation;
        startOperation();
    }
    else
    {
        // An operation is in progress...
        // ... we schedule the attach op for later
        nextOperation_ = new_operation;
    }
}

// Performs the current operation asynchronously, a indexingFinished
// signal will be received when it's finished.
void LogData::startOperation()
{
    if ( currentOperation_ )
    {
        LOG(logDEBUG) << "startOperation found something to do.";

        // Let the operation do its stuff
        currentOperation_->start( workerThread_ );
    }
}

//
// Slots
//

void LogData::fileChangedOnDisk( const QString& filename )
{
    LOG(logINFO) << "signalFileChanged " << filename.toStdString();

    const QString name = attached_file_->fileName();
    QFileInfo info( name );

    const auto file_size = indexing_data_.getSize();
    LOG(logDEBUG) << "current indexed fileSize=" << file_size;
    LOG(logDEBUG) << "info file_->size()=" << info.size();
    LOG(logDEBUG) << "attached_file_->size()=" << attached_file_->size();
    // In absence of any clearer information, we use the following size comparison
    // to determine whether we are following the same file or not (i.e. the file
    // has been moved and the inode we are following is now under a new name, if for
    // instance log has been rotated). We want to follow the name so we have to reopen
    // the file to ensure we are reading the right one.
    // This is a crude heuristic but necessary for notification services that do not
    // give details (e.g. kqueues)
    if ( ( info.size() != attached_file_->size() )
            || ( attached_file_->openMode() == QIODevice::NotOpen ) ) {
        LOG(logINFO) << "Inconsistent size, the file might have changed, re-opening";

        reOpenFile();

        // We don't force a (slow) full reindex as this routinely happens if
        // the file is appended quickly.
        // This means we can occasionally have false negatives (should be dealt with at
        // a lower level): e.g. if a new file is created with the same name as the old one
        // and with a size greater than the old one (should be rare in practice).
    }

    std::function<std::shared_ptr<LogDataOperation>()> newOperation;

    const auto real_file_size = attached_file_->size();

    if ( real_file_size < file_size ) {
        fileChangedOnDisk_ = Truncated;
        LOG(logINFO) << "File truncated";
        newOperation = std::make_shared<FullIndexOperation>;
    }
    else if ( real_file_size == file_size ) {
        LOG(logINFO) << "No change in file";
    }
    else if ( fileChangedOnDisk_ != DataAdded ) {
        fileChangedOnDisk_ = DataAdded;
        LOG(logINFO) << "New data on disk";
        newOperation = std::make_shared<PartialIndexOperation>;
    }

    if ( newOperation ) {
        enqueueOperation( newOperation() );
        lastModifiedDate_ = info.lastModified();
        emit fileChanged( fileChangedOnDisk_ );
    }
}

void LogData::indexingFinished( LoadingStatus status )
{
    LOG(logDEBUG) << "indexingFinished: " <<
        ( status == LoadingStatus::Successful ) <<
        ", found " << indexing_data_.getNbLines() << " lines.";

    if ( status == LoadingStatus::Successful ) {
        // Start watching we watch the file for updates
        fileChangedOnDisk_ = Unchanged;
        FileWatcher::getFileWatcher().addFile( attached_file_->fileName() );

        // Update the modified date/time if the file exists
        lastModifiedDate_ = QDateTime();
        QFileInfo fileInfo( *attached_file_ );
        if ( fileInfo.exists() )
            lastModifiedDate_ = fileInfo.lastModified();
    }

    // FIXME be cleverer here as a notification might have arrived whilst we
    // were indexing.
    fileChangedOnDisk_ = Unchanged;

    LOG(logDEBUG) << "Sending indexingFinished.";
    emit loadingFinished( status );

    // So now the operation is done, let's see if there is something
    // else to do, in which case, do it!
    assert( currentOperation_ );

    currentOperation_ = std::move( nextOperation_ );
    nextOperation_.reset();

    if ( currentOperation_ ) {
        LOG(logDEBUG) << "indexingFinished is performing the next operation";
        startOperation();
    }
}

//
// Implementation of virtual functions
//
LinesCount LogData::doGetNbLine() const
{
    return indexing_data_.getNbLines();
}

LineLength LogData::doGetMaxLength() const
{
    return indexing_data_.getMaxLength();
}

LineLength LogData::doGetLineLength( LineNumber line ) const
{
    if ( line >= indexing_data_.getNbLines() ) { return 0_length; /* exception? */ }

    return LineLength( doGetExpandedLineString( line ).length() );
}

void LogData::doSetDisplayEncoding( const char* encoding )
{
    LOG(logDEBUG) << "AbstractLogData::setDisplayEncoding: " << encoding;
    codec_ = QTextCodec::codecForName( encoding );

    const QTextCodec* currentIndexCodec = indexing_data_.getForcedEncoding();
    if (!currentIndexCodec) currentIndexCodec = indexing_data_.getEncodingGuess();

    if (codec_->mibEnum() != currentIndexCodec->mibEnum())
    {
        if (EncodingParameters(codec_) != EncodingParameters(currentIndexCodec)) {
            bool isGuessedCodec = codec_->mibEnum() == indexing_data_.getEncodingGuess()->mibEnum();
            reload( isGuessedCodec  ? nullptr : codec_);
        }
    }
}

QTextCodec* LogData::doGetDisplayEncoding() const
{
    return codec_;
}

QString LogData::doGetLineString( LineNumber line ) const
{
    if ( line >= indexing_data_.getNbLines() ) { return ""; /* exception? */ }

    fileMutex_.lock();

    attached_file_->seek( ( line.get() == 0 ) ? 0 : indexing_data_.getPosForLine( line - 1_lcount ).get() );
    const auto rawString = attached_file_->readLine();

    fileMutex_.unlock();

    auto string = codec_->toUnicode( rawString );
    string.chop( 1 );

    return string;
}

QString LogData::doGetExpandedLineString( LineNumber line ) const
{
    if ( line >= indexing_data_.getNbLines() ) { return ""; /* exception? */ }

    fileMutex_.lock();

    attached_file_->seek( ( line.get() == 0 ) ? 0 : indexing_data_.getPosForLine( line - 1_lcount).get() );
    const auto rawString = attached_file_->readLine();

    fileMutex_.unlock();

    auto string = untabify( codec_->toUnicode( rawString ) );
    string.chop( 1 );

    return string;
}

// Note this function is also called from the LogFilteredDataWorker thread, so
// data must be protected because they are changed in the main thread (by
// indexingFinished).
std::vector<QString> LogData::doGetLines( LineNumber first_line, LinesCount number ) const
{
    const auto last_line = first_line + number - 1_lcount;

    // LOG(logDEBUG) << "LogData::doGetLines first_line:" << first_line << " nb:" << number;

    if ( number.get() == 0 ) {
        return std::vector<QString>();
    }

    if ( last_line >= indexing_data_.getNbLines() ) {
        LOG(logWARNING) << "LogData::doGetLines Lines out of bound asked for";
        return std::vector<QString>(); /* exception? */
    }

    const auto first_byte = (first_line.get() == 0) ?
        0 : indexing_data_.getPosForLine( first_line - 1_lcount ).get();
    const auto last_byte  = indexing_data_.getPosForLine( last_line ).get();

    fileMutex_.lock();

    // LOG(logDEBUG) << "LogData::doGetLines first_byte:" << first_byte << " last_byte:" << last_byte;
    attached_file_->seek( first_byte );
    const auto blob = attached_file_->read( last_byte - first_byte );

    fileMutex_.unlock();

    std::vector<QString> list;
    list.reserve( number.get() );

    qint64 beginning = 0;
    qint64 end = 0;
    std::unique_ptr<QTextDecoder> decoder {codec_->makeDecoder()};
    for ( LineNumber line = first_line; (line <= last_line); ++line ) {
        end = indexing_data_.getPosForLine( line ).get() - first_byte;
        // LOG(logDEBUG) << "Getting line " << line << " beginning " << beginning << " end " << end;
        // LOG(logDEBUG) << "Line is: " << std::string( blob.data() + beginning, end - beginning - 1 );
        list.emplace_back( decoder->toUnicode( blob.data() + beginning,
                                        static_cast<LineLength::UnderlyingType>( end - beginning - 1 ) ) );

        beginning = end;
    }

    return list;
}

std::vector<QString> LogData::doGetExpandedLines( LineNumber first_line, LinesCount number ) const
{
    const auto last_line = first_line + number - 1_lcount;

    if ( number.get() == 0 ) {
        return std::vector<QString>();
    }

    if ( last_line >= indexing_data_.getNbLines() ) {
        LOG(logWARNING) << "LogData::doGetExpandedLines Lines out of bound asked for";
        return std::vector<QString>(); /* exception? */
    }

    fileMutex_.lock();

    const auto first_byte = (first_line.get() == 0) ?
        0 : indexing_data_.getPosForLine( first_line-1_lcount ).get();
    const auto last_byte  = indexing_data_.getPosForLine( last_line ).get();
    // LOG(logDEBUG) << "LogData::doGetExpandedLines first_byte:" << first_byte << " last_byte:" << last_byte;

    attached_file_->seek( first_byte );
    QByteArray blob = attached_file_->read( last_byte - first_byte );

    fileMutex_.unlock();

    std::vector<QString> list;
    list.reserve( number.get() );

    qint64 beginning = 0;
    qint64 end = 0;
    std::unique_ptr<QTextDecoder> decoder {codec_->makeDecoder()};
    for ( auto line = first_line; (line <= last_line); ++line ) {
        end = indexing_data_.getPosForLine( line ).get() - first_byte;
        // LOG(logDEBUG) << "Getting line " << line << " beginning " << beginning << " end " << end;
        // LOG(logDEBUG) << "Line is: " << std::string( blob.data() + beginning, end - beginning - 1 );

        list.emplace_back( untabify( decoder->toUnicode( blob.data() + beginning,
                                                  static_cast<LineLength::UnderlyingType>( end - beginning - 1 ) ) ) );
        beginning = end;
    }

    return list;
}

QTextCodec* LogData::getDetectedEncoding() const
{
    return indexing_data_.getEncodingGuess();
}

// Close and reopen the file.
// Used if we suspect the file has been moved (we follow the old
// inode but really want the one now associated with the name)
void LogData::reOpenFile()
{
    auto reopened = std::make_unique<QFile>( attached_file_->fileName() );
    reopened->open( QIODevice::ReadOnly );
    QMutexLocker locker( &fileMutex_ );
    attached_file_ = std::move( reopened );
}