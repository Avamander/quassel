/***************************************************************************
 *   Copyright (C) 2005-2012 by the Quassel Project                        *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
 ***************************************************************************/

#include "signalproxy.h"

#include <QObject>
#include <QIODevice>
#include <QAbstractSocket>
#include <QHostAddress>
#include <QHash>
#include <QMultiHash>
#include <QList>
#include <QSet>
#include <QDebug>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QRegExp>
#ifdef HAVE_SSL
#include <QSslSocket>
#endif
#include <QThread>
#include <QTime>
#include <QEvent>
#include <QCoreApplication>

#include "protocol.h"
#include "syncableobject.h"
#include "util.h"

using namespace Protocol;

class RemovePeerEvent : public QEvent
{
public:
    RemovePeerEvent(SignalProxy::AbstractPeer *peer) : QEvent(QEvent::Type(SignalProxy::RemovePeerEvent)), peer(peer) {}
    SignalProxy::AbstractPeer *peer;
};


// ==================================================
//  SignalRelay
// ==================================================
class SignalProxy::SignalRelay : public QObject
{
/* Q_OBJECT is not necessary or even allowed, because we implement
   qt_metacall ourselves (and don't use any other features of the meta
   object system)
*/
public:
    SignalRelay(SignalProxy *parent) : QObject(parent), _proxy(parent) {}
    inline SignalProxy *proxy() const { return _proxy; }

    int qt_metacall(QMetaObject::Call _c, int _id, void **_a);

    void attachSignal(QObject *sender, int signalId, const QByteArray &funcName);
    void detachSignal(QObject *sender, int signalId = -1);

private:
    struct Signal {
        QObject *sender;
        int signalId;
        QByteArray signature;
        Signal(QObject *sender, int sigId, const QByteArray &signature) : sender(sender), signalId(sigId), signature(signature) {}
        Signal() : sender(0), signalId(-1) {}
    };

    SignalProxy *_proxy;
    QHash<int, Signal> _slots;
};


void SignalProxy::SignalRelay::attachSignal(QObject *sender, int signalId, const QByteArray &funcName)
{
    // we ride without safetybelts here... all checking for valid method etc pp has to be done by the caller
    // all connected methodIds are offset by the standard methodCount of QObject
    int slotId;
    for (int i = 0;; i++) {
        if (!_slots.contains(i)) {
            slotId = i;
            break;
        }
    }

    QByteArray fn;
    if (!funcName.isEmpty()) {
        fn = QMetaObject::normalizedSignature(funcName);
    }
    else {
        fn = SIGNAL(fakeMethodSignature());
        fn = fn.replace("fakeMethodSignature()", sender->metaObject()->method(signalId).signature());
    }

    _slots[slotId] = Signal(sender, signalId, fn);

    QMetaObject::connect(sender, signalId, this, QObject::staticMetaObject.methodCount() + slotId);
}


void SignalProxy::SignalRelay::detachSignal(QObject *sender, int signalId)
{
    QHash<int, Signal>::iterator slotIter = _slots.begin();
    while (slotIter != _slots.end()) {
        if (slotIter->sender == sender && (signalId == -1 || slotIter->signalId == signalId)) {
            slotIter = _slots.erase(slotIter);
            if (signalId != -1)
                break;
        }
        else {
            slotIter++;
        }
    }
}


int SignalProxy::SignalRelay::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;

    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_slots.contains(_id)) {
            QObject *caller = sender();

            SignalProxy::ExtendedMetaObject *eMeta = proxy()->extendedMetaObject(caller->metaObject());
            Q_ASSERT(eMeta);

            const Signal &signal = _slots[_id];

            QVariantList params;

            const QList<int> &argTypes = eMeta->argTypes(signal.signalId);
            for (int i = 0; i < argTypes.size(); i++) {
                if (argTypes[i] == 0) {
                    qWarning() << "SignalRelay::qt_metacall(): received invalid data for argument number" << i << "of signal" << QString("%1::%2").arg(caller->metaObject()->className()).arg(caller->metaObject()->method(_id).signature());
                    qWarning() << "                            - make sure all your data types are known by the Qt MetaSystem";
                    return _id;
                }
                params << QVariant(argTypes[i], _a[i+1]);
            }

            proxy()->dispatch(RpcCall(signal.signature, params));
        }
        _id -= _slots.count();
    }
    return _id;
}


// ==================================================
//  SignalProxy
// ==================================================
SignalProxy::SignalProxy(QObject *parent)
    : QObject(parent)
{
    setProxyMode(Client);
    init();
}


SignalProxy::SignalProxy(ProxyMode mode, QObject *parent)
    : QObject(parent)
{
    setProxyMode(mode);
    init();
}


SignalProxy::~SignalProxy()
{
    QHash<QByteArray, ObjectId>::iterator classIter = _syncSlave.begin();
    while (classIter != _syncSlave.end()) {
        ObjectId::iterator objIter = classIter->begin();
        while (objIter != classIter->end()) {
            SyncableObject *obj = objIter.value();
            objIter = classIter->erase(objIter);
            obj->stopSynchronize(this);
        }
        classIter++;
    }
    _syncSlave.clear();

    removeAllPeers();
}


void SignalProxy::setProxyMode(ProxyMode mode)
{
    if (_peers.count()) {
        qWarning() << Q_FUNC_INFO << "Cannot change proxy mode while connected";
        return;
    }

    _proxyMode = mode;
    if (mode == Server)
        initServer();
    else
        initClient();
}


void SignalProxy::init()
{
    _heartBeatInterval = 0;
    _maxHeartBeatCount = 0;
    _signalRelay = new SignalRelay(this);
    setHeartBeatInterval(30);
    setMaxHeartBeatCount(2);
    _secure = false;
    updateSecureState();
}


void SignalProxy::initServer()
{
}


void SignalProxy::initClient()
{
    attachSlot("__objectRenamed__", this, SLOT(objectRenamed(QByteArray,QString,QString)));
}


void SignalProxy::setHeartBeatInterval(int secs)
{
    if (_heartBeatInterval != secs) {
        _heartBeatInterval = secs;
        emit heartBeatIntervalChanged(secs);
    }
}


void SignalProxy::setMaxHeartBeatCount(int max)
{
    if (_maxHeartBeatCount != max) {
        _maxHeartBeatCount = max;
        emit maxHeartBeatCountChanged(max);
    }
}


bool SignalProxy::addPeer(AbstractPeer *peer)
{
    if (!peer)
        return false;

    if (_peers.contains(peer))
        return true;

    if (!peer->isOpen()) {
        qWarning("SignalProxy: peer needs to be open!");
        return false;
    }

    if (proxyMode() == Client) {
        if (!_peers.isEmpty()) {
            qWarning("SignalProxy: only one peer allowed in client mode!");
            return false;
        }
        connect(peer, SIGNAL(lagUpdated(int)), SIGNAL(lagUpdated(int)));
    }

    connect(peer, SIGNAL(disconnected()), SLOT(removePeerBySender()));
    connect(peer, SIGNAL(secureStateChanged(bool)), SLOT(updateSecureState()));

    if (!peer->parent())
        peer->setParent(this);

    _peers.insert(peer);

    peer->setSignalProxy(this);

    if (_peers.count() == 1)
        emit connected();

    updateSecureState();
    return true;
}


void SignalProxy::removeAllPeers()
{
    Q_ASSERT(proxyMode() == Server || _peers.count() <= 1);
    // wee need to copy that list since we modify it in the loop
    QSet<AbstractPeer *> peers = _peers;
    foreach(AbstractPeer *peer, peers) {
        removePeer(peer);
    }
}


void SignalProxy::removePeer(AbstractPeer *peer)
{
    if (!peer) {
        qWarning() << Q_FUNC_INFO << "Trying to remove a null peer!";
        return;
    }

    if (_peers.isEmpty()) {
        qWarning() << "SignalProxy::removePeer(): No peers in use!";
        return;
    }

    if (!_peers.contains(peer)) {
        qWarning() << "SignalProxy: unknown Peer" << peer;
        return;
    }

    disconnect(peer, 0, this, 0);
    peer->setSignalProxy(0);

    _peers.remove(peer);
    emit peerRemoved(peer);

    if (peer->parent() == this)
        peer->deleteLater();

    updateSecureState();

    if (_peers.isEmpty())
        emit disconnected();
}


void SignalProxy::removePeerBySender()
{
    removePeer(qobject_cast<SignalProxy::AbstractPeer *>(sender()));
}


void SignalProxy::renameObject(const SyncableObject *obj, const QString &newname, const QString &oldname)
{
    if (proxyMode() == Client)
        return;

    const QMetaObject *meta = obj->syncMetaObject();
    const QByteArray className(meta->className());
    objectRenamed(className, newname, oldname);

    dispatch(RpcCall("__objectRenamed__", QVariantList() << className << newname << oldname));
}


void SignalProxy::objectRenamed(const QByteArray &classname, const QString &newname, const QString &oldname)
{
    if (_syncSlave.contains(classname) && _syncSlave[classname].contains(oldname) && oldname != newname) {
        SyncableObject *obj = _syncSlave[classname][newname] = _syncSlave[classname].take(oldname);
        requestInit(obj);
    }
}


const QMetaObject *SignalProxy::metaObject(const QObject *obj)
{
    if (const SyncableObject *syncObject = qobject_cast<const SyncableObject *>(obj))
        return syncObject->syncMetaObject();
    else
        return obj->metaObject();
}


SignalProxy::ExtendedMetaObject *SignalProxy::extendedMetaObject(const QMetaObject *meta) const
{
    if (_extendedMetaObjects.contains(meta))
        return _extendedMetaObjects[meta];
    else
        return 0;
}


SignalProxy::ExtendedMetaObject *SignalProxy::createExtendedMetaObject(const QMetaObject *meta, bool checkConflicts)
{
    if (!_extendedMetaObjects.contains(meta)) {
        _extendedMetaObjects[meta] = new ExtendedMetaObject(meta, checkConflicts);
    }
    return _extendedMetaObjects[meta];
}


bool SignalProxy::attachSignal(QObject *sender, const char *signal, const QByteArray &sigName)
{
    const QMetaObject *meta = sender->metaObject();
    QByteArray sig(meta->normalizedSignature(signal).mid(1));
    int methodId = meta->indexOfMethod(sig.constData());
    if (methodId == -1 || meta->method(methodId).methodType() != QMetaMethod::Signal) {
        qWarning() << "SignalProxy::attachSignal(): No such signal" << signal;
        return false;
    }

    createExtendedMetaObject(meta);
    _signalRelay->attachSignal(sender, methodId, sigName);

    disconnect(sender, SIGNAL(destroyed(QObject *)), this, SLOT(detachObject(QObject *)));
    connect(sender, SIGNAL(destroyed(QObject *)), this, SLOT(detachObject(QObject *)));
    return true;
}


bool SignalProxy::attachSlot(const QByteArray &sigName, QObject *recv, const char *slot)
{
    const QMetaObject *meta = recv->metaObject();
    int methodId = meta->indexOfMethod(meta->normalizedSignature(slot).mid(1));
    if (methodId == -1 || meta->method(methodId).methodType() == QMetaMethod::Method) {
        qWarning() << "SignalProxy::attachSlot(): No such slot" << slot;
        return false;
    }

    createExtendedMetaObject(meta);

    QByteArray funcName = QMetaObject::normalizedSignature(sigName.constData());
    _attachedSlots.insert(funcName, qMakePair(recv, methodId));

    disconnect(recv, SIGNAL(destroyed(QObject *)), this, SLOT(detachObject(QObject *)));
    connect(recv, SIGNAL(destroyed(QObject *)), this, SLOT(detachObject(QObject *)));
    return true;
}


void SignalProxy::synchronize(SyncableObject *obj)
{
    createExtendedMetaObject(obj, true);

    // attaching as slave to receive sync Calls
    QByteArray className(obj->syncMetaObject()->className());
    _syncSlave[className][obj->objectName()] = obj;

    if (proxyMode() == Server) {
        obj->setInitialized();
        emit objectInitialized(obj);
    }
    else {
        if (obj->isInitialized())
            emit objectInitialized(obj);
        else
            requestInit(obj);
    }

    obj->synchronize(this);
}


void SignalProxy::detachObject(QObject *obj)
{
    detachSignals(obj);
    detachSlots(obj);
}


void SignalProxy::detachSignals(QObject *sender)
{
    _signalRelay->detachSignal(sender);
}


void SignalProxy::detachSlots(QObject *receiver)
{
    SlotHash::iterator slotIter = _attachedSlots.begin();
    while (slotIter != _attachedSlots.end()) {
        if (slotIter.value().first == receiver) {
            slotIter = _attachedSlots.erase(slotIter);
        }
        else
            slotIter++;
    }
}


void SignalProxy::stopSynchronize(SyncableObject *obj)
{
    // we can't use a className here, since it might be effed up, if we receive the call as a result of a decon
    // gladly the objectName() is still valid. So we have only to iterate over the classes not each instance! *sigh*
    QHash<QByteArray, ObjectId>::iterator classIter = _syncSlave.begin();
    while (classIter != _syncSlave.end()) {
        if (classIter->contains(obj->objectName()) && classIter.value()[obj->objectName()] == obj) {
            classIter->remove(obj->objectName());
            break;
        }
        classIter++;
    }
    obj->stopSynchronize(this);
}


template<class T>
void SignalProxy::dispatch(const T &protoMessage)
{
    foreach (AbstractPeer *peer, _peers) {
        if (peer->isOpen())
            peer->dispatch(protoMessage);
        else
            QCoreApplication::postEvent(this, new ::RemovePeerEvent(peer));
    }
}


void SignalProxy::handle(SignalProxy::AbstractPeer *peer, const SyncMessage &syncMessage)
{
    if (!_syncSlave.contains(syncMessage.className()) || !_syncSlave[syncMessage.className()].contains(syncMessage.objectName())) {
        qWarning() << QString("no registered receiver for sync call: %1::%2 (objectName=\"%3\"). Params are:").arg(syncMessage.className(), syncMessage.slotName(), syncMessage.objectName())
                   << syncMessage.params();
        return;
    }

    SyncableObject *receiver = _syncSlave[syncMessage.className()][syncMessage.objectName()];
    ExtendedMetaObject *eMeta = extendedMetaObject(receiver);
    if (!eMeta->slotMap().contains(syncMessage.slotName())) {
        qWarning() << QString("no matching slot for sync call: %1::%2 (objectName=\"%3\"). Params are:").arg(syncMessage.className(), syncMessage.slotName(), syncMessage.objectName())
                   << syncMessage.params();
        return;
    }

    int slotId = eMeta->slotMap()[syncMessage.slotName()];
    if (proxyMode() != eMeta->receiverMode(slotId)) {
        qWarning("SignalProxy::handleSync(): invokeMethod for \"%s\" failed. Wrong ProxyMode!", eMeta->methodName(slotId).constData());
        return;
    }

    QVariant returnValue((QVariant::Type)eMeta->returnType(slotId));
    if (!invokeSlot(receiver, slotId, syncMessage.params(), returnValue)) {
        qWarning("SignalProxy::handleSync(): invokeMethod for \"%s\" failed ", eMeta->methodName(slotId).constData());
        return;
    }

    if (returnValue.type() != QVariant::Invalid && eMeta->receiveMap().contains(slotId)) {
        int receiverId = eMeta->receiveMap()[slotId];
        QVariantList returnParams;
        if (eMeta->argTypes(receiverId).count() > 1)
            returnParams << syncMessage.params();
        returnParams << returnValue;
        peer->dispatch(SyncMessage(syncMessage.className(), syncMessage.objectName(), eMeta->methodName(receiverId), returnParams));
    }

    // send emit update signal
    invokeSlot(receiver, eMeta->updatedRemotelyId());
}


void SignalProxy::handle(SignalProxy::AbstractPeer *peer, const InitRequest &initRequest)
{
   if (!_syncSlave.contains(initRequest.className())) {
        qWarning() << "SignalProxy::handleInitRequest() received initRequest for unregistered Class:"
                   << initRequest.className();
        return;
    }

    if (!_syncSlave[initRequest.className()].contains(initRequest.objectName())) {
        qWarning() << "SignalProxy::handleInitRequest() received initRequest for unregistered Object:"
                   << initRequest.className() << initRequest.objectName();
        return;
    }

    SyncableObject *obj = _syncSlave[initRequest.className()][initRequest.objectName()];
    peer->dispatch(InitData(initRequest.className(), initRequest.objectName(), initData(obj)));
}


void SignalProxy::handle(SignalProxy::AbstractPeer *peer, const InitData &initData)
{
    Q_UNUSED(peer)

    if (!_syncSlave.contains(initData.className())) {
        qWarning() << "SignalProxy::handleInitData() received initData for unregistered Class:"
                   << initData.className();
        return;
    }

    if (!_syncSlave[initData.className()].contains(initData.objectName())) {
        qWarning() << "SignalProxy::handleInitData() received initData for unregistered Object:"
                   << initData.className() << initData.objectName();
        return;
    }

    SyncableObject *obj = _syncSlave[initData.className()][initData.objectName()];
    setInitData(obj, initData.initData());
}


void SignalProxy::handle(SignalProxy::AbstractPeer *peer, const RpcCall &rpcCall)
{
    Q_UNUSED(peer)

    QObject *receiver;
    int methodId;
    SlotHash::const_iterator slot = _attachedSlots.constFind(rpcCall.slotName());
    while (slot != _attachedSlots.constEnd() && slot.key() == rpcCall.slotName()) {
        receiver = (*slot).first;
        methodId = (*slot).second;
        if (!invokeSlot(receiver, methodId, rpcCall.params())) {
            ExtendedMetaObject *eMeta = extendedMetaObject(receiver);
            qWarning("SignalProxy::handleSignal(): invokeMethod for \"%s\" failed ", eMeta->methodName(methodId).constData());
        }
        ++slot;
    }
}


bool SignalProxy::invokeSlot(QObject *receiver, int methodId, const QVariantList &params, QVariant &returnValue)
{
    ExtendedMetaObject *eMeta = extendedMetaObject(receiver);
    const QList<int> args = eMeta->argTypes(methodId);
    const int numArgs = params.count() < args.count()
                        ? params.count()
                        : args.count();

    if (eMeta->minArgCount(methodId) > params.count()) {
        qWarning() << "SignalProxy::invokeSlot(): not enough params to invoke" << eMeta->methodName(methodId);
        return false;
    }

    void *_a[] = { 0,           // return type...
                   0, 0, 0, 0, 0, // and 10 args - that's the max size qt can handle with signals and slots
                   0, 0, 0, 0, 0 };

    // check for argument compatibility and build params array
    for (int i = 0; i < numArgs; i++) {
        if (!params[i].isValid()) {
            qWarning() << "SignalProxy::invokeSlot(): received invalid data for argument number" << i << "of method" << QString("%1::%2()").arg(receiver->metaObject()->className()).arg(receiver->metaObject()->method(methodId).signature());
            qWarning() << "                            - make sure all your data types are known by the Qt MetaSystem";
            return false;
        }
        if (args[i] != QMetaType::type(params[i].typeName())) {
            qWarning() << "SignalProxy::invokeSlot(): incompatible param types to invoke" << eMeta->methodName(methodId);
            return false;
        }
        _a[i+1] = const_cast<void *>(params[i].constData());
    }

    if (returnValue.type() != QVariant::Invalid)
        _a[0] = const_cast<void *>(returnValue.constData());

    Qt::ConnectionType type = QThread::currentThread() == receiver->thread()
                              ? Qt::DirectConnection
                              : Qt::QueuedConnection;

    if (type == Qt::DirectConnection) {
        return receiver->qt_metacall(QMetaObject::InvokeMetaMethod, methodId, _a) < 0;
    }
    else {
        qWarning() << "Queued Connections are not implemented yet";
        // note to self: qmetaobject.cpp:990 ff
        return false;
    }
}


bool SignalProxy::invokeSlot(QObject *receiver, int methodId, const QVariantList &params)
{
    QVariant ret;
    return invokeSlot(receiver, methodId, params, ret);
}


void SignalProxy::requestInit(SyncableObject *obj)
{
    if (proxyMode() == Server || obj->isInitialized())
        return;

    dispatch(InitRequest(obj->syncMetaObject()->className(), obj->objectName()));
}


QVariantMap SignalProxy::initData(SyncableObject *obj) const
{
    return obj->toVariantMap();
}


void SignalProxy::setInitData(SyncableObject *obj, const QVariantMap &properties)
{
    if (obj->isInitialized())
        return;
    obj->fromVariantMap(properties);
    obj->setInitialized();
    emit objectInitialized(obj);
    invokeSlot(obj, extendedMetaObject(obj)->updatedRemotelyId());
}


void SignalProxy::customEvent(QEvent *event)
{
    switch ((int)event->type()) {
    case RemovePeerEvent: {
        ::RemovePeerEvent *e = static_cast< ::RemovePeerEvent *>(event);
        removePeer(e->peer);
        event->accept();
        break;
    }

    default:
        qWarning() << Q_FUNC_INFO << "Received unknown custom event:" << event->type();
        return;
    }
}


void SignalProxy::sync_call__(const SyncableObject *obj, SignalProxy::ProxyMode modeType, const char *funcname, va_list ap)
{
    // qDebug() << obj << modeType << "(" << _proxyMode << ")" << funcname;
    if (modeType != _proxyMode)
        return;

    ExtendedMetaObject *eMeta = extendedMetaObject(obj);

    QVariantList params;

    const QList<int> &argTypes = eMeta->argTypes(eMeta->methodId(QByteArray(funcname)));

    for (int i = 0; i < argTypes.size(); i++) {
        if (argTypes[i] == 0) {
            qWarning() << Q_FUNC_INFO << "received invalid data for argument number" << i << "of signal" << QString("%1::%2").arg(eMeta->metaObject()->className()).arg(funcname);
            qWarning() << "        - make sure all your data types are known by the Qt MetaSystem";
            return;
        }
        params << QVariant(argTypes[i], va_arg(ap, void *));
    }

    dispatch(SyncMessage(eMeta->metaObject()->className(), obj->objectName(), QByteArray(funcname), params));
}


void SignalProxy::disconnectDevice(QIODevice *dev, const QString &reason)
{
    if (!reason.isEmpty())
        qWarning() << qPrintable(reason);
    QAbstractSocket *sock  = qobject_cast<QAbstractSocket *>(dev);
    if (sock)
        qWarning() << qPrintable(tr("Disconnecting")) << qPrintable(sock->peerAddress().toString());
    dev->close();
}


void SignalProxy::dumpProxyStats()
{
    QString mode;
    if (proxyMode() == Server)
        mode = "Server";
    else
        mode = "Client";

    int slaveCount = 0;
    foreach(ObjectId oid, _syncSlave.values())
    slaveCount += oid.count();

    qDebug() << this;
    qDebug() << "              Proxy Mode:" << mode;
    qDebug() << "          attached Slots:" << _attachedSlots.count();
    qDebug() << " number of synced Slaves:" << slaveCount;
    qDebug() << "number of Classes cached:" << _extendedMetaObjects.count();
}


void SignalProxy::updateSecureState()
{
    bool wasSecure = _secure;

    _secure = !_peers.isEmpty();
    foreach (const AbstractPeer *peer,  _peers) {
        _secure &= peer->isSecure();
    }

    if (wasSecure != _secure)
        emit secureStateChanged(_secure);
}


// ==================================================
//  ExtendedMetaObject
// ==================================================
SignalProxy::ExtendedMetaObject::ExtendedMetaObject(const QMetaObject *meta, bool checkConflicts)
    : _meta(meta),
    _updatedRemotelyId(_meta->indexOfSignal("updatedRemotely()"))
{
    for (int i = 0; i < _meta->methodCount(); i++) {
        if (_meta->method(i).methodType() != QMetaMethod::Slot)
            continue;

        if (QByteArray(_meta->method(i).signature()).contains('*'))
            continue;  // skip methods with ptr params

        QByteArray method = methodName(_meta->method(i));
        if (method.startsWith("init"))
            continue;  // skip initializers

        if (_methodIds.contains(method)) {
            /* funny... moc creates for methods containing default parameters multiple metaMethod with separate methodIds.
               we don't care... we just need the full fledged version
             */
            const QMetaMethod &current = _meta->method(_methodIds[method]);
            const QMetaMethod &candidate = _meta->method(i);
            if (current.parameterTypes().count() > candidate.parameterTypes().count()) {
                int minCount = candidate.parameterTypes().count();
                QList<QByteArray> commonParams = current.parameterTypes().mid(0, minCount);
                if (commonParams == candidate.parameterTypes())
                    continue;  // we already got the full featured version
            }
            else {
                int minCount = current.parameterTypes().count();
                QList<QByteArray> commonParams = candidate.parameterTypes().mid(0, minCount);
                if (commonParams == current.parameterTypes()) {
                    _methodIds[method] = i; // use the new one
                    continue;
                }
            }
            if (checkConflicts) {
                qWarning() << "class" << meta->className() << "contains overloaded methods which is currently not supported!";
                qWarning() << " - " << _meta->method(i).signature() << "conflicts with" << _meta->method(_methodIds[method]).signature();
            }
            continue;
        }
        _methodIds[method] = i;
    }
}


const SignalProxy::ExtendedMetaObject::MethodDescriptor &SignalProxy::ExtendedMetaObject::methodDescriptor(int methodId)
{
    if (!_methods.contains(methodId)) {
        _methods[methodId] = MethodDescriptor(_meta->method(methodId));
    }
    return _methods[methodId];
}


const QHash<int, int> &SignalProxy::ExtendedMetaObject::receiveMap()
{
    if (_receiveMap.isEmpty()) {
        QHash<int, int> receiveMap;

        QMetaMethod requestSlot;
        QByteArray returnTypeName;
        QByteArray signature;
        QByteArray methodName;
        QByteArray params;
        int paramsPos;
        int receiverId;
        const int methodCount = _meta->methodCount();
        for (int i = 0; i < methodCount; i++) {
            requestSlot = _meta->method(i);
            if (requestSlot.methodType() != QMetaMethod::Slot)
                continue;

            returnTypeName = requestSlot.typeName();
            if (QMetaType::Void == (QMetaType::Type)returnType(i))
                continue;

            signature = QByteArray(requestSlot.signature());
            if (!signature.startsWith("request"))
                continue;

            paramsPos = signature.indexOf('(');
            if (paramsPos == -1)
                continue;

            methodName = signature.left(paramsPos);
            params = signature.mid(paramsPos);

            methodName = methodName.replace("request", "receive");
            params = params.left(params.count() - 1) + ", " + returnTypeName + ")";

            signature = QMetaObject::normalizedSignature(methodName + params);
            receiverId = _meta->indexOfSlot(signature);

            if (receiverId == -1) {
                signature = QMetaObject::normalizedSignature(methodName + "(" + returnTypeName + ")");
                receiverId = _meta->indexOfSlot(signature);
            }

            if (receiverId != -1) {
                receiveMap[i] = receiverId;
            }
        }
        _receiveMap = receiveMap;
    }
    return _receiveMap;
}


QByteArray SignalProxy::ExtendedMetaObject::methodName(const QMetaMethod &method)
{
    QByteArray sig(method.signature());
    return sig.left(sig.indexOf("("));
}


QString SignalProxy::ExtendedMetaObject::methodBaseName(const QMetaMethod &method)
{
    QString methodname = QString(method.signature()).section("(", 0, 0);

    // determine where we have to chop:
    int upperCharPos;
    if (method.methodType() == QMetaMethod::Slot) {
        // we take evertyhing from the first uppercase char if it's slot
        upperCharPos = methodname.indexOf(QRegExp("[A-Z]"));
        if (upperCharPos == -1)
            return QString();
        methodname = methodname.mid(upperCharPos);
    }
    else {
        // and if it's a signal we discard everything from the last uppercase char
        upperCharPos = methodname.lastIndexOf(QRegExp("[A-Z]"));
        if (upperCharPos == -1)
            return QString();
        methodname = methodname.left(upperCharPos);
    }

    methodname[0] = methodname[0].toUpper();

    return methodname;
}


SignalProxy::ExtendedMetaObject::MethodDescriptor::MethodDescriptor(const QMetaMethod &method)
    : _methodName(SignalProxy::ExtendedMetaObject::methodName(method)),
    _returnType(QMetaType::type(method.typeName()))
{
    // determine argTypes
    QList<QByteArray> paramTypes = method.parameterTypes();
    QList<int> argTypes;
    for (int i = 0; i < paramTypes.count(); i++) {
        argTypes.append(QMetaType::type(paramTypes[i]));
    }
    _argTypes = argTypes;

    // determine minArgCount
    QString signature(method.signature());
    _minArgCount = method.parameterTypes().count() - signature.count("=");

    _receiverMode = (_methodName.startsWith("request"))
                    ? SignalProxy::Server
                    : SignalProxy::Client;
}
