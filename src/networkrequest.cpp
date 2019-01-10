#include "NetworkRequest.h"
#include "NetworkDownloadRequest.h"
#include "NetworkUploadRequest.h"
#include "NetworkCommonRequest.h"
#include "NetworkMTDownloadRequest.h"


std::unique_ptr<NetworkRequest> NetworkRequestFactory::createRequestInstance(const RequestType& eType, bool bMTDownloadMode, QObject *parent)
{
	std::unique_ptr<NetworkRequest> pRequest;
	switch (eType)
	{
	case eTypeDownload:
		{
			if (bMTDownloadMode)
			{
				pRequest.reset(new NetworkMTDownloadRequest(parent));
			}
			else
			{
				pRequest.reset(new NetworkDownloadRequest(parent));
			}
		}
		break;
	case eTypeUpload:
		{
		pRequest.reset(new NetworkUploadRequest(parent));
		}
		break;
	case eTypePost:
	case eTypeGet:
	case eTypePut:
	case eTypeDelete:
	case eTypeHead:
		{
		pRequest.reset(new NetworkCommonRequest(parent));
		}
		break;
	/*New type add to here*/
	default:
		break;
	}
	return pRequest;
}