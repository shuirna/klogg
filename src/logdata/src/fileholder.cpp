#include "fileholder.h"

#include <filewatcher.h>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "log.h"
#include <QtCore/QFileInfo>

namespace {
void openFileByHandle( QFile* file )
{
    bool openedByHandle = false;

#ifdef Q_OS_WIN
    //
    // The following code is adapted from Qt's QFSFileEnginePrivate::nativeOpen()
    // by including the FILE_SHARE_DELETE share mode.
    //

    // Enable full sharing.
    DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    int accessRights = GENERIC_READ;
    DWORD creationDisp = OPEN_EXISTING;

    // Create the file handle.
    SECURITY_ATTRIBUTES securityAtts = { sizeof( SECURITY_ATTRIBUTES ), NULL, FALSE };
    HANDLE fileHandle
        = CreateFileW( (const wchar_t*)file->fileName().utf16(), accessRights, shareMode,
                       &securityAtts, creationDisp, FILE_ATTRIBUTE_NORMAL, NULL );

    if ( fileHandle != INVALID_HANDLE_VALUE ) {
        // Convert the HANDLE to an fd and pass it to QFile's foreign-open
        // function. The fd owns the handle, so when QFile later closes
        // the fd the handle will be closed too.
        int fd = _open_osfhandle( (intptr_t)fileHandle, _O_RDONLY );
        if ( fd != -1 ) {
            openedByHandle = file->open( fd, QIODevice::ReadOnly, QFile::AutoCloseHandle );
        }
        else {
            LOG( logWARNING ) << "Failed to open file by handle " << file->fileName();
            ::CloseHandle( fileHandle );
        }
    }
    else {
        LOG( logWARNING ) << "Failed to open file by handle " << file->fileName();
    }
#endif
    if ( !openedByHandle ) {
        file->open( QIODevice::ReadOnly );
    }
}
} // namespace

FileHolder::FileHolder( bool keepClosed )
    : file_mutex_{ QMutex::Recursive }
    , keep_closed_{ keepClosed }
{
}

FileHolder::~FileHolder()
{
    // Remove the current file from the watch list
    if ( attached_file_ )
        FileWatcher::getFileWatcher().removeFile( file_name_ );
}

FileId FileHolder::getFileId()
{
    QMutexLocker locker( &file_mutex_ );
    return attached_file_id_;
}

qint64 FileHolder::size()
{
    QMutexLocker locker( &file_mutex_ );
    return attached_file_ ? attached_file_->size() : 0;
}

bool FileHolder::isOpen()
{
    QMutexLocker locker( &file_mutex_ );
    return attached_file_ ? attached_file_->openMode() != QIODevice::NotOpen : false;
}

void FileHolder::open( const QString& fileName )
{
    QMutexLocker locker( &file_mutex_ );
    file_name_ = fileName;

    LOG( logDEBUG ) << "open file " << file_name_ << " keep closed " << keep_closed_;

    if ( !keep_closed_ ) {
        counter_ = 1;
        reOpenFile();
    }
}

void FileHolder::lock()
{
    file_mutex_.lock();
}

void FileHolder::unlock()
{
    file_mutex_.unlock();
}

void FileHolder::attachReader()
{
    QMutexLocker locker( &file_mutex_ );

    if ( keep_closed_ && counter_ == 0 ) {
        LOG( logDEBUG ) << "fist reader opened for " << file_name_;
        reOpenFile();
    }

    counter_++;

    LOG( logDEBUG ) << "has " << counter_ << " readers for " << file_name_;

}

void FileHolder::detachReader()
{
    QMutexLocker locker( &file_mutex_ );
    if ( counter_ > 0 ) {
        counter_--;
    }

    if ( keep_closed_ && counter_ == 0 ) {
        attached_file_->close();
        LOG( logDEBUG ) << "last reader closed for " << file_name_;
    }
}

void FileHolder::reOpenFile()
{
    LOG( logDEBUG ) << "reopen " << file_name_;

    auto reopened = std::make_unique<QFile>( file_name_ );
    openFileByHandle( reopened.get() );

    QMutexLocker locker( &file_mutex_ );
    attached_file_ = std::move( reopened );
    attached_file_id_ = FileId::getFileId( file_name_ );
}

QFile* FileHolder::getFile()
{
    return attached_file_.get();
}

FileId FileId::getFileId( const QString& filename )
{
#ifdef Q_OS_WIN
    DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    int accessRights = GENERIC_READ;
    DWORD creationDisp = OPEN_EXISTING;

    // Create the file handle.
    SECURITY_ATTRIBUTES securityAtts = { sizeof( SECURITY_ATTRIBUTES ), NULL, FALSE };

    HANDLE fileHandle
        = CreateFileW( (const wchar_t*)filename.utf16(), accessRights, shareMode, &securityAtts,
                       creationDisp, FILE_FLAG_BACKUP_SEMANTICS, NULL );

    if ( fileHandle == INVALID_HANDLE_VALUE ) {
        LOG( logDEBUG ) << "Failed to get file info for " << filename.toStdString() << ", gle "
                        << ::GetLastError();
        return FileId{};
    }

    using FileHandleGuard = std::unique_ptr<void, decltype( &CloseHandle )>;
    auto fileHandleGuard = FileHandleGuard{ fileHandle, CloseHandle };

    BY_HANDLE_FILE_INFORMATION info;
    if ( !::GetFileInformationByHandle( fileHandle, &info ) ) {
        LOG( logDEBUG ) << "Failed to get file info for " << filename.toStdString() << ", gle "
                        << ::GetLastError();
        return FileId{};
    }

    ULARGE_INTEGER fileIndex = { info.nFileIndexLow, info.nFileIndexHigh };
    return FileId{ fileIndex.QuadPart, static_cast<uint64_t>( info.dwVolumeSerialNumber ) };
#else
    struct stat info;
    if ( lstat( filename.toUtf8().constData(), &info ) != 0 ) {
        LOG( logDEBUG ) << "Failed to get file info for " << filename.toStdString();
        return FileId{};
    }

    return FileId{ info.st_ino, static_cast<uint64_t>( info.st_dev ) };
#endif
}
