﻿/*
* Licensed to the Apache Software Foundation (ASF) under one
* or more contributor license agreements.  See the NOTICE file
* distributed with this work for additional information
* regarding copyright ownership.  The ASF licenses this file
* to you under the Apache License, Version 2.0 (the
* "License"); you may not use this file except in compliance
* with the License.  You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an
* "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
* KIND, either express or implied.  See the License for the
* specific language governing permissions and limitations
* under the License.
*/

#include "util/logger.h"
#include "sync.h"
#include "t_redis.h"
#include "t_zset.h"
#include "t_hash.h"
#include "ttlmanager.h"


Sync::Sync(RedisProxy* proxy, const char* master, int port)
{
    m_reconnectInterval = 5 * 1000;
    m_syncInterval = 5 * 1000;
    m_proxy = proxy;
    m_masterAddr = HostAddress(master, port);
    m_slaveIndexFileName = "MASTER_INFO";
}

Sync::~Sync()
{
    m_socket.close();
}

void Sync::setSyncInterval(int t)
{
    m_syncInterval = t;
}

void Sync::setReconnectInterval(int t)
{
    if (t > 0) {
        m_reconnectInterval = t;
    }
}


void Sync::repairConnect(void)
{
    m_socket.close();
    m_socket = TcpSocket::createTcpSocket();
    while (true) {
        if (!m_socket.connect(m_masterAddr)) {
            Logger::log(Logger::Message, "reconnect to master ip:%s,port:%d failed ..",
                        m_masterAddr.ip(),m_masterAddr.port());
            Thread::sleep(m_reconnectInterval);
            continue;
        }
        Logger::log(Logger::Message, "connect to master ip:%s,port:%d succeed ..",
                    m_masterAddr.ip(),m_masterAddr.port());
        break;
    }
}



bool Sync::_onSetCommand(Binlog::LogItem* item, LeveldbCluster* db)
{
    char* key = item->keyBuffer();
    int keySize = item->key_size;
    char* value = item->valueBuffer();
    int valueSize = item->value_size;
    short type = *((short*)key);
    switch (type) {
    case T_KV:
    case T_Ttl:
    case T_List:
    case T_ListElement:
        return db->setValue(XObject(key, keySize), XObject(value, valueSize));
    case T_ZSet:
    case T_Set:
    case T_Hash: {
        HashKeyInfo info;
        THash::unmakeHashKey(key, keySize, &info);
        LeveldbCluster::WriteOption op;
        op.mapping_key = XObject(info.name.data, info.name.len);
        return db->setValue(XObject(key, keySize), XObject(value, valueSize), op);
    }

    default:
        return false;
    }
}

bool Sync::_onDelCommand(Binlog::LogItem* item, LeveldbCluster* db)
{
    char* key = item->keyBuffer();
    int keySize = item->key_size;
    short type = *((short*)key);

    switch (type) {
    case T_KV:
    case T_Ttl:
    case T_List:
    case T_ListElement:
        return db->remove(XObject(key, keySize));
    case T_ZSet:
    case T_Set:
    case T_Hash: {
        HashKeyInfo info;
        THash::unmakeHashKey(key, keySize, &info);
        LeveldbCluster::WriteOption op;
        op.mapping_key = XObject(info.name.data, info.name.len);
        return db->remove(XObject(key, keySize), op);
    }

    default:
        return false;
    }
}


void Sync::run(void)
{
    m_masterSyncInfo.clear();
    //这里是读取binlog同步的位置, 第一行文件名 第二行读取偏移量
    bool ok = TextConfigFile::read(m_slaveIndexFileName, m_masterSyncInfo);
    if (ok) {
        if (m_masterSyncInfo.size() != 2)  {
            Logger::log(Logger::Warning, "MASTER_INFO file invalid");
            return;
        }
    } else {
        //如果没有读取到，默认值
        m_masterSyncInfo.clear();
        m_masterSyncInfo.push_back(" ");
        m_masterSyncInfo.push_back("-1");
    }
    m_socket = TcpSocket::createTcpSocket();

    if (!m_socket.connect(m_masterAddr)) {
        Logger::log(Logger::Warning, "connect to master (%s:%d) failed ..",
                    m_masterAddr.ip(),m_masterAddr.port());
        return;
    }

    char sendBuf[256];
    memset(sendBuf, '\0', sizeof(sendBuf));

    //不断发送_SYNC命令来同步
    while (true) {

        //1.组装_SYNC命令
        int sendLenth = sprintf(sendBuf,"*3\r\n$6\r\n__SYNC\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                                (int)m_masterSyncInfo[0].length(), m_masterSyncInfo[0].c_str(),
                (int)m_masterSyncInfo[1].length(), m_masterSyncInfo[1].c_str());
        int sendBytes = m_socket.send(sendBuf, sendLenth);
        if (sendBytes < 0) {
            repairConnect();
            continue;
        }

        int len = 0;
        bool is_error = false;
        //todo 这里recvBUf完全可以复用，不需要每次新分配
        char recvBuf[sizeof(BinlogSyncStream)] = {0};
        //2. 接收master信息 --- 这里的recvSize是固定的大小吗?
        // 应该是先接收一个SyncStream, 然后才是真正的数据
        while (len != sizeof(recvBuf)) {
            int recvSize = 0;
            recvSize = m_socket.recv(recvBuf + len, sizeof(recvBuf) - len);
            if (recvSize <= 0) {
                is_error = true;
                break;
            }
            len += recvSize;
        }

        //3. 接收发生错误，需要修复连接
        if (is_error) {
            repairConnect();
            continue;
        }

        IOBuffer iobuf;
        BinlogSyncStream* pStream = (BinlogSyncStream*)recvBuf;
        if (len == sizeof(BinlogSyncStream)) {
            iobuf.reserve(pStream->streamSize);
            iobuf.append(recvBuf, len);
            //4. 这里接收真正的数据，大小为streamSize
            while (len < pStream->streamSize) {
                IOBuffer::DirectCopy cp = iobuf.beginCopy();
                int recv_size = m_socket.recv(cp.address, cp.maxsize);
                if (recv_size <= 0) {
                    is_error = true;
                    break;
                }
                iobuf.endCopy(recv_size);
                len += recv_size;
            }
        }

        if (is_error) {
            repairConnect();
            continue;
        }

        //把ioBuf里面的数据转换下
        BinlogSyncStream* stream = (BinlogSyncStream*)(iobuf.data());
        //Logger::log(Logger::Message, "[RECV] %d,%s,cnt=%d,last_pos=%d",
        //            stream->streamSize,stream->srcFileName,stream->logItemCount, stream->lastUpdatePos);

        if (stream->error != BinlogSyncStream::NoError) {
            Logger::log(Logger::Error, "ERROR(%d) sync thread stopped",
                        stream->error, stream->errorMsg);
            return;
        }
        char posBuf[20] = {0};
        sprintf(posBuf, "%d", stream->lastUpdatePos);

        m_masterSyncInfo.clear();
        m_masterSyncInfo.push_back(stream->srcFileName);
        m_masterSyncInfo.push_back(posBuf);
        TextConfigFile::write(m_slaveIndexFileName, m_masterSyncInfo);

        int i = 0;
        for (Binlog::LogItem* item = stream->firstLogItem();
             i < stream->logItemCount; item = stream->nextLogItem(item), ++i)
        {
            LeveldbCluster* db = m_proxy->leveldbCluster();
            switch (item->type)
            {
            case Binlog::LogItem::SET:
                _onSetCommand(item, db);
                break;
            case Binlog::LogItem::DEL:
                _onDelCommand(item, db);
                break;
            default:
                break;
            }
        }

        Thread::sleep(m_syncInterval);
    }
}




