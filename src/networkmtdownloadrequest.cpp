#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QNetworkAccessManager>
#include "NetworkMTDownloadRequest.h"
#include "NetworkManager.h"
#include "Log4cplusWrapper.h"
#include "ClassMemoryTracer.h"


#define NET_FILE_WRITE

NetworkMTDownloadRequest::NetworkMTDownloadRequest(QObject *parent /* = nullptr */)
	: NetworkRequest(parent)
	, m_pNetworkManager(nullptr)
	, m_nThreadCount(0)
	, m_nSuccessNum(0)
	, m_nFailedNum(0)
	, m_bytesReceived(0)
	, m_bytesTotal(0)
	, m_nFileSize(-1)
{
}

NetworkMTDownloadRequest::~NetworkMTDownloadRequest()
{
	abort();

	if (m_pNetworkManager)
	{
		m_pNetworkManager->deleteLater();
		m_pNetworkManager = nullptr;
	}
}

void NetworkMTDownloadRequest::abort()
{
	LOG_FUN("");
	m_bAbortManual = true;

	foreach(std::shared_ptr<Downloader> pDownloader, m_mapDownloader)
	{
		if (pDownloader.get())
		{
			pDownloader->abort();
			pDownloader.reset();
		}
	}
	m_mapDownloader.clear();
	m_mapBytes.clear();
	m_bytesTotal = 0;
	m_bytesReceived = 0;
}

bool NetworkMTDownloadRequest::createLocalFile()
{
	m_strError.clear();
	//取下载文件保存目录
	QString strSaveDir = QDir::toNativeSeparators(m_request.strReqArg);
	if (!strSaveDir.isEmpty())
	{
		QDir dir;
		if (!dir.exists(strSaveDir))
		{
			if (!dir.mkpath(strSaveDir))
			{
				m_strError = QStringLiteral("Error: QDir::mkpath failed! Dir(%1)").arg(strSaveDir);
				qWarning() << m_strError;
				LOG_INFO(m_strError.toStdWString());
				return false;
			}
		}
	}
	else
	{
		m_strError = QLatin1String("Error: RequestTask::strRequestArg is empty!");
		qWarning() << m_strError;
		LOG_INFO(m_strError.toStdWString());
		return false;
	}
	if (!strSaveDir.endsWith("\\"))
	{
		strSaveDir.append("\\");
	}

	//取下载保存的文件名
	QString strFileName;
	if (!m_request.strSaveFileName.isEmpty())
	{
		strFileName = m_request.strSaveFileName;
	}
	else
	{
		if (redirected())
		{
			strFileName = m_redirectUrl.fileName();
		}
		else
		{
			strFileName = m_request.url.fileName();
		}
	}

	if (strFileName.isEmpty())
	{
		m_strError = QLatin1String("Error: fileName is empty!");
		qWarning() << m_strError;
		LOG_INFO(m_strError.toStdWString());
		return false;
	}

	//如果文件存在，关闭文件并移除
	const QString& strFilePath = QDir::toNativeSeparators(strSaveDir + strFileName);
	if (QFile::exists(strFilePath))
	{
		QFile file(strFilePath);
		if (!removeFile(&file))
		{
			m_strError = QStringLiteral("Error: QFile::remove(%1) - %2").arg(strFilePath).arg(file.errorString());
			qWarning() << m_strError;
			LOG_INFO(m_strError.toStdWString());
			return false;
		}
	}

	m_strDstFilePath = strFilePath;
	HANDLE hFile = CreateFileW(m_strDstFilePath.toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE,
							   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
							   FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile != nullptr && hFile != INVALID_HANDLE_VALUE)
	{
		LARGE_INTEGER li = {0};
		li.QuadPart = m_nFileSize;
		if (!SetFilePointerEx(hFile, li, &li, FILE_BEGIN))
		{
			qDebug() << "SetFilePointerEx total:" << GetLastError();
			return false;
		}
		CloseHandle(hFile);
	}
	else
	{
		qDebug() << "CreateFileW total:" << GetLastError();
		return false;
	}

	return true;
}

//用获取下载文件的长度
bool NetworkMTDownloadRequest::requestFileSize(QUrl url)
{
	if (!url.isValid())
	{
		return false;
	}
	m_nFileSize = -1;
	m_url = url;

	if (nullptr == m_pNetworkManager)
	{
		m_pNetworkManager = new QNetworkAccessManager;
	}
	QNetworkRequest request(url);
	request.setRawHeader("Accept-Encoding", "identity");
	//request.setRawHeader("Accept-Encoding", "gzip");

#ifndef QT_NO_SSL
	if (isHttpsProxy(url.scheme()))
	{
		// 发送https请求前准备工作;
		QSslConfiguration conf = request.sslConfiguration();
		conf.setPeerVerifyMode(QSslSocket::VerifyNone);
		conf.setProtocol(QSsl::TlsV1SslV3);
		request.setSslConfiguration(conf);
	}
#endif

	m_pReply = m_pNetworkManager->head(request);
	if (m_pReply)
	{
		connect(m_pReply, SIGNAL(finished()), this, SLOT(onGetFileSizeFinished()));
		connect(m_pReply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(onGetFileSizeError(QNetworkReply::NetworkError)));
	}
	return true;
}

void NetworkMTDownloadRequest::start()
{
	m_bAbortManual = false;

	m_nSuccessNum = 0;
	m_nFailedNum = 0;
	m_bytesReceived = 0;
	m_bytesTotal = 0;

	m_nThreadCount = m_request.nDownloadThreadCount;
	if (m_nThreadCount < 1)
	{
		m_nThreadCount = 1;
	}
	if (m_nThreadCount > 10)
	{
		m_nThreadCount = 10;
	}

	if (nullptr == m_pNetworkManager)
	{
		m_pNetworkManager = new QNetworkAccessManager;
	}

	bool b = requestFileSize(m_request.url);
	if (!b)
	{
		m_strError = QStringLiteral("Invalid Url").toUtf8();
		emit requestFinished(false, m_strError.toUtf8());
	}
}

void NetworkMTDownloadRequest::startMTDownload()
{
	if (m_bAbortManual)
	{
		return;
	}

	if (m_nFileSize <= 0)
	{
		m_nThreadCount = 1;
		m_bytesTotal = 0;
		qDebug() << QStringLiteral("服务器未返回Content-Length");
		LOG_INFO(QStringLiteral("服务器未返回Content-Length").toStdWString());
	}

	if (createLocalFile())
	{
		if (m_bAbortManual)
		{
			return;
		}

		foreach(std::shared_ptr<Downloader> pDownloader, m_mapDownloader)
		{
			if (pDownloader.get())
			{
				pDownloader->abort();
				pDownloader.reset();
			}
		}
		m_mapDownloader.clear();
		m_mapBytes.clear();

		//将文件分成n段，用异步的方式下载
		for (int i = 0; i < m_nThreadCount; i++)
		{
			qint64 start = 0;
			qint64 end = -1;
			if (m_nThreadCount > 1 && m_nFileSize > 0)
			{
				//先算出每段的开头和结尾（HTTP协议所需要的信息）
				start = m_nFileSize * i / m_nThreadCount;
				end = m_nFileSize * (i + 1) / m_nThreadCount;
				if (i == m_nThreadCount - 1)
				{
					end--;
				}
			}

			//分段下载该文件
			std::shared_ptr<Downloader> downloader = std::make_shared<Downloader>(i, this);
			connect(downloader.get(), SIGNAL(downloadFinished(int, bool, const QString&)), 
					this, SLOT(onSubPartFinished(int, bool, const QString&)));
			connect(downloader.get(), SIGNAL(downloadProgress(int, qint64, qint64)), 
					this, SLOT(onSubPartDownloadProgress(int, qint64, qint64)));
			if (downloader->startDownload(m_request.url, m_strDstFilePath, m_pNetworkManager,
				start, end, m_request.bShowProgress))
			{
				m_mapDownloader.insert(i, downloader);
				m_mapBytes.insert(i, ProgressData());
			}
			else
			{
				abort();
				m_strError = QStringLiteral("Subpart %1 startDownload() failed!").arg(i);
				LOG_ERROR(m_strError.toStdWString());
				emit requestFinished(false, m_strError.toUtf8());
				return;
			}
		}
	}
	else
	{
		emit requestFinished(false, m_strError.toUtf8());
	}
}

void NetworkMTDownloadRequest::onSubPartFinished(int index, bool bSuccess, const QString& strErr)
{
	if (m_bAbortManual)
	{
		return;
	}

	if (bSuccess)
	{
		m_nSuccessNum++;
	}
	else
	{
		m_nFailedNum++;
		if (m_nFailedNum == 1)
		{
			abort();
		}
		if (m_strError.isEmpty())
		{
			m_strError = strErr;
		}
	}

	//如果完成数等于文件段数，则说明文件下载成功；失败数大于0，说明下载失败
	if (m_nSuccessNum == m_nThreadCount || m_nFailedNum == 1)
	{
		emit requestFinished((m_nFailedNum == 0), m_strError.toUtf8());
		qDebug() << "MT Download finished. success:" << (m_nFailedNum == 0);
	}

	if (m_mapDownloader.contains(index))
	{
		std::shared_ptr<Downloader> pDownloader = m_mapDownloader.value(index);
		m_mapDownloader.remove(index);
		if (pDownloader.get())
		{
			pDownloader.reset();
		}
	}
}

void NetworkMTDownloadRequest::onSubPartDownloadProgress(int index, qint64 bytesReceived, qint64 bytesTotal)
{
	if (m_bAbortManual || bytesReceived <= 0 || bytesTotal <= 0)
		return;

	if (m_mapBytes.contains(index))
	{
		//qDebug() << "Part:" << index << " progress:" << bytesReceived << "/" << bytesTotal;

		qint64 bytesRevIncreased = 0;//本次接收增加的字节数
		qint64 bytesTotalIncreased = 0;//本次总增加的字节数

		qint64 bytesRev = m_mapBytes.value(index).bytesReceived;
		if (bytesReceived > bytesRev)
		{
			bytesRevIncreased = bytesReceived - bytesRev;
			m_bytesReceived += bytesRevIncreased;
		}
		m_mapBytes[index].bytesReceived = bytesReceived;

		if (m_nThreadCount <= 1 || m_nFileSize <= 0)
		{
			qint64 bytesTo = m_mapBytes.value(index).bytesTotal;
			if (bytesTotal > bytesTo)
			{
				bytesTotalIncreased = bytesTotal - bytesTo;
				m_bytesTotal += bytesTotalIncreased;
			}
			m_mapBytes[index].bytesTotal = bytesTotal;
		}

		if (m_bytesTotal > 0 && m_bytesReceived > 0)
		{
			if (NetworkManager::isInstantiated())
			{
				NetworkProgressEvent *event = new NetworkProgressEvent;
				event->uiId = m_request.uiId;
				event->uiBatchId = m_request.uiBatchId;
				event->iBtyes = m_bytesReceived;
				event->iTotalBtyes = m_bytesTotal;
				QCoreApplication::postEvent(NetworkManager::globalInstance(), event);
			}
		}
	}
}

void NetworkMTDownloadRequest::onGetFileSizeFinished()
{
	bool bSuccess = (m_pReply->error() == QNetworkReply::NoError);
	int statusCode = m_pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (isHttpProxy(m_url.scheme()) || isHttpsProxy(m_url.scheme()))
	{
		bSuccess = bSuccess && (statusCode >= 200 && statusCode < 300);
	}
	if (!bSuccess)
	{
		if (statusCode == 301 || statusCode == 302)
		{//301,302重定向
			const QVariant& redirectionTarget = m_pReply->attribute(QNetworkRequest::RedirectionTargetAttribute);
			if (!redirectionTarget.isNull())
			{
				const QUrl& redirectUrl = m_url.resolved(redirectionTarget.toUrl());
				qDebug() << "url:" << m_url.toString() << "redirectUrl:" << redirectUrl.toString();
				if (m_redirectUrl.isValid())
				{
					m_pReply->deleteLater();
					m_pReply = nullptr;
					requestFileSize(redirectUrl);
				}
			}
		}
		else if (statusCode != 200 && statusCode != 0)
		{
			qDebug() << "HttpStatusCode:" << statusCode;
		}
	}
	else
	{
		if (m_request.uiBatchId == 0)
		{
			foreach(const QByteArray& header, m_pReply->rawHeaderList())
			{
				qDebug() << header << ":" << m_pReply->rawHeader(header);
			}
		}
		QVariant var = m_pReply->header(QNetworkRequest::ContentLengthHeader);
		m_nFileSize = var.toLongLong();
		m_bytesTotal = m_nFileSize;
		qDebug() << "MT File size:" << m_nFileSize;
		LOG_INFO("MT File size: " << m_nFileSize);
		startMTDownload();
	}
}

void NetworkMTDownloadRequest::onGetFileSizeError(QNetworkReply::NetworkError code)
{
	LOG_FUN("");
	Q_UNUSED(code);
	QNetworkReply *pNetworkReply = qobject_cast<QNetworkReply *>(sender());
	if (pNetworkReply)
	{
		qDebug() << "NetworkBigFleDownloadRequest::onError" << pNetworkReply->errorString();
		LOG_ERROR("url: " << m_request.url.toString().toStdWString()
				  << "; error: " << pNetworkReply->errorString().toStdString());
		m_strError = pNetworkReply->errorString();
	}
}

bool NetworkMTDownloadRequest::fileAccessible(QFile *pFile) const
{
	return (nullptr != pFile && pFile->exists());
}

bool NetworkMTDownloadRequest::removeFile(QFile *pFile)
{
	if (fileAccessible(pFile))
	{
		pFile->close();
		return pFile->remove();
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////
Downloader::Downloader(int index, QObject *parent)
	: QObject(parent)
	, m_nIndex(index)
	, m_pNetworkManager(nullptr)
	, m_pNetworkReply(nullptr)
	, m_bAbortManual(false)
	, m_nStartPoint(0)
	, m_nEndPoint(0)
	, m_hFile(0)
{
	TRACE_CLASS_CONSTRUCTOR(Downloader);
}

Downloader::~Downloader()
{
	TRACE_CLASS_DESTRUCTOR(Downloader);
	abort();
}

void Downloader::abort()
{
	if (m_pNetworkReply)
	{
		m_bAbortManual = true;
		m_pNetworkReply->abort();
		m_pNetworkReply->deleteLater();
		m_pNetworkReply = nullptr;
		m_pNetworkManager = nullptr;
		if (m_hFile)
		{
			CloseHandle(m_hFile);
			m_hFile = nullptr;
		}
	}
}

bool Downloader::startDownload(const QUrl &url,
							   const QString& strDstFile,
							   QNetworkAccessManager* pNetworkManager,
							   qint64 startPoint,
							   qint64 endPoint,
							   bool bShowProgress)
{
	if (nullptr == pNetworkManager || !url.isValid() || strDstFile.isEmpty())
		return false;

	m_bAbortManual = false;

	m_url = url;
	m_pNetworkManager = pNetworkManager;
	m_nStartPoint = startPoint;
	m_nEndPoint = endPoint;
	m_bShowProgress = bShowProgress;

	m_strDstFilePath = strDstFile;
	m_hFile = CreateFileW(strDstFile.toStdWString().c_str(), GENERIC_WRITE,
						  FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (m_hFile != nullptr && m_hFile != INVALID_HANDLE_VALUE)
	{
		LARGE_INTEGER li = {0};
		li.HighPart = (startPoint << 32);
		li.LowPart = (DWORD)startPoint;
		if (!SetFilePointerEx(m_hFile, li, nullptr, FILE_BEGIN))
		{
			qDebug() << "SetFilePointerEx:" << GetLastError();
			return false;
		}
	}
	else
	{
		qDebug() << "CreateFileW:" << GetLastError();
		return false;
	}

	//根据HTTP协议，写入RANGE头部，说明请求文件的范围
	QNetworkRequest request;
	request.setUrl(url);
	QString range;
	range.sprintf("Bytes=%lld-%lld", m_nStartPoint, m_nEndPoint);
	request.setRawHeader("Range", range.toLocal8Bit());
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
	request.setRawHeader("Accept-Encoding", "gzip");

#ifndef QT_NO_SSL
	if (isHttpsProxy(url.scheme()))
	{
		// 发送https请求前准备工作;
		QSslConfiguration conf = request.sslConfiguration();
		conf.setPeerVerifyMode(QSslSocket::VerifyNone);
		conf.setProtocol(QSsl::TlsV1SslV3);
		request.setSslConfiguration(conf);
	}
#endif

	qDebug() << "Part" << m_nIndex << "start, Range:" << range;
	LOG_INFO("Part " << m_nIndex << " start, Range: " << range.toStdString());
	m_pNetworkReply = m_pNetworkManager->get(request);
	if (m_pNetworkReply)
	{
		connect(m_pNetworkReply, SIGNAL(finished()), this, SLOT(onFinished()));
		connect(m_pNetworkReply, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
		connect(m_pNetworkReply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(onError(QNetworkReply::NetworkError)));
		if (bShowProgress)
		{
			connect(m_pNetworkReply, &QNetworkReply::downloadProgress, this, [=](qint64 bytesReceived, qint64 bytesTotal) {
				if (m_bAbortManual || bytesReceived < 0 || bytesTotal < 0)
					return;
				emit downloadProgress(m_nIndex, bytesReceived, bytesTotal);
			});
		}
	}
	else
	{
		qDebug() << "error";
	}
	return true;
}

void Downloader::onReadyRead()
{
	if (m_pNetworkReply
		&& m_pNetworkReply->error() == QNetworkReply::NoError
		&& m_pNetworkReply->isOpen())
	{
		if (m_hFile != nullptr)
		{
			const QByteArray& bytesRev = m_pNetworkReply->readAll();
			if (!bytesRev.isEmpty())
			{
				DWORD byteWritten = 0;
				if (!WriteFile(m_hFile, bytesRev.data(), bytesRev.size(), &byteWritten, nullptr))
				{
					qDebug() << "WriteFile:" << GetLastError();
				}
				if (byteWritten != bytesRev.size())
				{
					qCritical() << "error write!" << byteWritten << bytesRev.size();
				}
			}
		}
	}
}

void Downloader::onFinished()
{
	try
	{
		bool bSuccess = (m_pNetworkReply->error() == QNetworkReply::NoError);
		int statusCode = m_pNetworkReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (isHttpProxy(m_url.scheme()) || isHttpsProxy(m_url.scheme()))
		{
			bSuccess = bSuccess && (statusCode >= 200 && statusCode < 300);
		}
		if (!bSuccess)
		{
			if (statusCode == 301 || statusCode == 302)
			{//301,302重定向
				const QVariant& redirectionTarget = m_pNetworkReply->attribute(QNetworkRequest::RedirectionTargetAttribute);
				if (!redirectionTarget.isNull())
				{//如果网址跳转重新请求
					const QUrl& redirectUrl = m_url.resolved(redirectionTarget.toUrl());
					if (redirectUrl.isValid() && redirectUrl != m_url)
					{
						qDebug() << "url:" << m_url.toString() << "redirectUrl:" << redirectUrl.toString();
						LOG_INFO("url: " << m_url.toString().toStdWString() << "; redirectUrl:" << redirectUrl.toString().toStdWString());

						m_pNetworkReply->abort();
						m_pNetworkReply->deleteLater();
						m_pNetworkReply = nullptr;
						if (m_hFile)
						{
							CloseHandle(m_hFile);
							m_hFile = nullptr;
						}

						startDownload(redirectUrl, m_strDstFilePath, m_pNetworkManager.data(),
									  m_nStartPoint, m_nEndPoint, m_bShowProgress);
						return;
					}
				}
			}
			else if (statusCode != 200 && statusCode != 0)
			{
				//qDebug() << "HttpStatusCode: " << statusCode;
			}
		}
		else
		{
			if (m_hFile)
			{
				FlushFileBuffers(m_hFile);
			}
		}

		qDebug() << "Part" << m_nIndex << "[result]" << bSuccess;
		LOG_INFO("Part " << m_nIndex << " [result] " << bSuccess);

		m_pNetworkReply->deleteLater();
		m_pNetworkReply = nullptr;
		if (m_hFile)
		{
			CloseHandle(m_hFile);
			m_hFile = nullptr;
		}

		emit downloadFinished(m_nIndex, bSuccess, m_strError);
	}
	catch (std::exception& ex)
	{
		qDebug() << "Part" << m_nIndex << QString::fromUtf8(ex.what());
	}
}

void Downloader::onError(QNetworkReply::NetworkError code)
{
	LOG_FUN("");
	Q_UNUSED(code);
	qDebug() << "Part" << m_nIndex << m_pNetworkReply->errorString();
	LOG_ERROR("[Part " << m_nIndex << "]" << m_pNetworkReply->errorString().toStdString());
	m_strError = m_pNetworkReply->errorString();
}