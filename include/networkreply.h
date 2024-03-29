﻿#ifndef NETWORKREPLY_H
#define NETWORKREPLY_H

#include <QObject>
#include "networkdef.h"
#include "network_global.h"

// 对象会自动销毁，不要主动销毁
class NETWORK_EXPORT NetworkReply : public QObject
{
    Q_OBJECT

public:
    NetworkReply(bool bBatch, QObject *parent = 0);
    ~NetworkReply();

    bool isBatchRequest() const { return m_bBatch; }

    virtual bool event(QEvent *) Q_DECL_OVERRIDE;

Q_SIGNALS:
    void requestFinished(const RequestTask &);

protected:
    void replyResult(const RequestTask& request, bool bDestroy = false);
    friend class NetworkManager;
    friend class NetworkManagerPrivate;

private:
    bool m_bBatch;
};

#endif // NETWORKREPLY_H