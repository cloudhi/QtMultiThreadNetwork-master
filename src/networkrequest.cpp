#include "NetworkRequest.h"
#include "NetworkDownloadRequest.h"
#include "NetworkUploadRequest.h"
#include "NetworkCommonRequest.h"
#include "NetworkMTDownloadRequest.h"


std::unique_ptr<NetworkRequest> NetworkRequestFactory::create(const RequestType& eType)
{
	std::unique_ptr<NetworkRequest> pRequest;
	switch (eType)
	{
	case eTypeDownload:
		{
			pRequest.reset(new NetworkDownloadRequest());
		}
		break;
	case eTypeMTDownload:
		{
			pRequest.reset(new NetworkMTDownloadRequest());
		}
		break;
	case eTypeUpload:
		{
			pRequest.reset(new NetworkUploadRequest());
		}
		break;
	case eTypePost:
	case eTypeGet:
	case eTypePut:
	case eTypeDelete:
	case eTypeHead:
		{
			pRequest.reset(new NetworkCommonRequest());
		}
		break;
		/*New type add to here*/
	default:
		break;
	}
	return pRequest;
}