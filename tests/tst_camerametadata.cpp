#include "camerametadata.h"
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>
#include <functional>

namespace {
const QString Prefix="1/4294048770/";
QJsonObject snapshot(int revision=1,const QString &name="Cloud camera",const QString &identity=QString(64,'a')) {
    return {{"0",identity},{"1","12345678-1234-4234-8234-123456789abc"},{"2",revision},{"3",name},
        {"4",QString(64,'b')},{"5","Cloud home"},{"6","10"},{"7","Living room"},{"8","2026-10-24T08:00:00.000Z"}};
}
QJsonObject attributes(bool writable=true) {
    return {{"1/29/0",QJsonArray{QJsonObject{{"deviceType",0x142},{"revision",1}}}},
        {"1/29/1",QJsonArray{29,0x551,0x553,4294048770LL}}, {Prefix+"0",snapshot()},
        {Prefix+"1",QJsonArray{QJsonObject{{"0","10"},{"1","Living room"}},QJsonObject{{"0","20"},{"1","Study"}}}},
        {Prefix+"2",QJsonObject{{"0",true},{"1",writable},{"2",writable},{"3",writable}}},
        {Prefix+"3",true},{Prefix+"4",true},{Prefix+"5",false},{Prefix+"6",""}};
}
QJsonObject incompleteSnapshot(bool observed = false) {
    auto result=snapshot(2); for (const auto &tag : {"3","5","6","7"}) result.insert(tag,QString());
    result.insert("8",observed?QStringLiteral("2026-10-24T08:00:00.001Z"):QString()); return result;
}
QJsonObject incompleteAttributes(bool observed = false) {
    auto result=attributes(); result.insert(Prefix+"0",incompleteSnapshot(observed)); result.insert(Prefix+"1",QJsonArray{});
    result.insert(Prefix+"4",false); result.insert(Prefix+"6","metadata_incomplete"); return result;
}
class Server:public QObject {
public:
    QWebSocketServer server{"Synthetic metadata protocol",QWebSocketServer::NonSecureMode};
    QWebSocket *peer=nullptr; QJsonObject attrs=attributes(); QList<QJsonObject> requests,commands;
    std::function<bool(const QJsonObject &)> intercept;
    Server() {
        server.listen(QHostAddress::LocalHost,0);
        connect(&server,&QWebSocketServer::newConnection,this,[this] {
            peer=server.nextPendingConnection(); peer->setParent(this);
            connect(peer,&QWebSocket::textMessageReceived,this,[this](const QString &value) {
                const auto request=QJsonDocument::fromJson(value.toUtf8()).object(); requests.append(request);
                const auto cmd=request.value("command").toString();
                if (cmd=="device_command") commands.append(request);
                if (intercept && intercept(request)) return;
                if (cmd=="start_listening" || cmd=="get_nodes") reply(request,QJsonArray{node()});
                else if (cmd=="get_node") reply(request,node());
                else if (cmd=="read_attribute") replyRead(request);
            });
            send({{"schema_version",14},{"min_supported_schema_version",11},{"fabric_id","synthetic-fabric"}});
        });
    }
    QJsonObject node()const { return {{"node_id",7},{"available",true},{"attributes",attrs}}; }
    QUrl url()const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    void send(const QJsonObject &o) { if(peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))); }
    void reply(const QJsonObject &request,const QJsonValue &value) { send({{"message_id",request.value("message_id")},{"result",value}}); }
    void replyRead(const QJsonObject &r) { QJsonObject result; for(const auto &p:r.value("args").toObject().value("attribute_path").toArray()) if(attrs.contains(p.toString())) result.insert(p.toString(),attrs.value(p.toString())); reply(r,result); }
    void report(const QJsonObject &s) { attrs.insert(Prefix+"0",s); send({{"event","attribute_updated"},{"data",QJsonArray{7,Prefix+"0",s}}}); }
    int count(const QString &cmd)const { int n=0;for(const auto&r:requests)if(r.value("command")==cmd)++n;return n; }
};
}
class CameraMetadataTest:public QObject {
    Q_OBJECT
private slots:
    void strictAtomicParsersUseVerifiedTagBasedShape() {
        CameraMetadataSnapshot parsed; QVERIFY(parseCameraMetadataSnapshot(snapshot(),&parsed)); QCOMPARE(parsed.name,QStringLiteral("Cloud camera"));
        const auto original=parsed;
        for(const auto &entry:QList<QPair<QString,QJsonValue>>{{"2",1.5},{"2",9007199254740992.0},{"2",-1},{"2","2"},{"0","bad"},{"1","bad"},{"8","2026-10-24"},{"3",QString(161,'x')},{"3","line\nsecret"}}) {
            auto altered=snapshot(); altered.insert(entry.first,entry.second); QVERIFY(!parseCameraMetadataSnapshot(altered,&parsed)); QVERIFY(parsed==original);
        }
        auto boundary=snapshot(); boundary.insert("2",9007199254740991.0); boundary.insert("3",QString(53,QChar(0x4e2d))); QVERIFY(parseCameraMetadataSnapshot(boundary,&parsed)); QCOMPARE(parsed.revision,quint64(9007199254740991ULL));
        boundary.insert("3",QString(54,QChar(0x4e2d))); QVERIFY(!parseCameraMetadataSnapshot(boundary,&parsed));
        QVERIFY(!parseCameraMetadataSnapshot(QJsonObject{{"identity",QString(64,'a')}},&parsed));
        QList<CameraMetadataRoom> rooms; QVERIFY(parseCameraMetadataRooms(attributes().value(Prefix+"1"),&rooms)); QCOMPARE(rooms.size(),2);
        const auto before=rooms; QVERIFY(!parseCameraMetadataRooms(QJsonArray{QJsonObject{{"0","same"},{"1","A"}},QJsonObject{{"0","same"},{"1","B"}}},&rooms)); QVERIFY(rooms==before);
        CameraMetadataCapabilities caps; QVERIFY(parseCameraMetadataCapabilities(attributes().value(Prefix+"2"),&caps)); QVERIFY(caps.rename);
        QVERIFY(!parseCameraMetadataCapabilities(QJsonObject{{"0",true},{"1",1},{"2",true},{"3",true}},&caps));
    }
    void unicodePairsAndUtf8BoundsAreValidatedBeforeInvoke() {
        CameraMetadataSnapshot parsed; auto value=snapshot();
        const auto high=QString(QChar(0xd83d)),low=QString(QChar(0xdcf7));
        for (const auto &invalid : {high,low,high+QStringLiteral("x")}) { value.insert("3",invalid); QVERIFY(!parseCameraMetadataSnapshot(value,&parsed)); }
        const auto emoji=high+low; value.insert("3",emoji.repeated(40)); QVERIFY(parseCameraMetadataSnapshot(value,&parsed)); QCOMPARE(parsed.name.toUtf8().size(),160);
        value.insert("3",emoji.repeated(41)); QVERIFY(!parseCameraMetadataSnapshot(value,&parsed));
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QVERIFY(!manager.rename(d,high)); const auto baseline=manager.state(d).snapshot;
        QSignalSpy responses(&client,&MatterClient::cameraMetadataResponse);
        client.invokeCameraMetadata(d,"rename",{{"expectedIdentity",baseline.identity},{"expectedSourceEpoch",baseline.sourceEpoch},{"expectedRevision",double(baseline.revision)},{"name",low}});
        QTRY_COMPARE(responses.size(),1); QVERIFY(!responses.first().at(1).toBool()); QCOMPARE(server.commands.size(),0);
    }
    void provisionalSnapshotsRequireClearedDataAndUtcMilliseconds() {
        CameraMetadataSnapshot parsed; QVERIFY(parseCameraMetadataSnapshot(incompleteSnapshot(),&parsed)); QVERIFY(parsed.observedAt.isEmpty()); QCOMPARE(parsed.identity,QString(64,'a'));
        const auto baseline=parsed;
        auto partial=incompleteSnapshot(); partial.insert("3","residual"); QVERIFY(!parseCameraMetadataSnapshot(partial,&parsed)); QVERIFY(parsed==baseline);
        QVERIFY(parseCameraMetadataSnapshot(incompleteSnapshot(true),&parsed));
        for (const auto &invalid : {"2026-10-24T08:00:00Z","2026-10-24T08:00:00.001+00:00","2026-02-30T08:00:00.001Z"}) {
            partial=incompleteSnapshot(); partial.insert("8",invalid); QVERIFY(!parseCameraMetadataSnapshot(partial,&parsed));
        }
    }
    void incompletePublicationClearsNamesRoomsAndOldMirror() {
        QTemporaryDir directory; Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,directory.path()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QCOMPARE(manager.state(d).snapshot.name,QStringLiteral("Cloud camera")); server.attrs=incompleteAttributes(true);
        server.send({{"event","node_updated"},{"data",server.node()}});
        QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy&&manager.state(d).lastError=="metadata_incomplete");
        const auto state=manager.state(d); QVERIFY(state.snapshot.name.isEmpty()&&state.snapshot.homeName.isEmpty()&&state.snapshot.roomName.isEmpty()&&state.snapshot.roomId.isEmpty());
        QVERIFY(state.rooms.isEmpty()); QVERIFY(!state.available); QVERIFY(!manager.canRename(d)&&!manager.canMoveRoom(d)&&!manager.canSetSync(d));
        const auto files=QDir(directory.path()).entryList({"*.json"},QDir::Files); QCOMPARE(files.size(),1); QFile file(QDir(directory.path()).filePath(files.first())); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!QJsonDocument::fromJson(file.readAll()).object().value("mirrors").toObject().contains(QString(64,'a')));
        server.send({{"event","attribute_updated"},{"data",QJsonArray{7,"1/6/0",true}}}); QTest::qWait(30); QVERIFY(manager.state(d).snapshot.name.isEmpty()); QCOMPARE(server.commands.size(),0);
    }
    void initialIncompleteWithNoObservationIsKnownAndReadOnly() {
        Server server; server.attrs=incompleteAttributes(); MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        const auto state=manager.state(d); QVERIFY(state.snapshot.observedAt.isEmpty()&&state.snapshot.name.isEmpty()&&state.rooms.isEmpty());
        QVERIFY(!state.available); QCOMPARE(state.lastError,QStringLiteral("metadata_incomplete"));
        QVERIFY(!manager.rename(d,"No")&&!manager.moveRoom(d,"10")&&!manager.setSync(d,false)); QCOMPARE(server.commands.size(),0);
    }
    void firstObservationCanBeRecoveredByExplicitRefresh_data() {
        QTest::addColumn<QString>("reason");
        QTest::newRow("first read pending") << QString();
        QTest::newRow("source unavailable") << QStringLiteral("metadata_unavailable");
        QTest::newRow("source incomplete") << QStringLiteral("metadata_incomplete");
    }
    void firstObservationCanBeRecoveredByExplicitRefresh() {
        QFETCH(QString, reason);
        Server server; server.attrs=incompleteAttributes(); server.attrs.insert(Prefix+"6",reason);
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first();
        QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QVERIFY(cameraMetadataUnavailable(manager.state(d)));
        QVERIFY(!manager.canRename(d)&&!manager.canMoveRoom(d)&&!manager.canSetSync(d));
        const auto baseline=manager.state(d).snapshot;
        QJsonObject guards{{"expectedIdentity",baseline.identity},{"expectedSourceEpoch",baseline.sourceEpoch},{"expectedRevision",double(baseline.revision)}};
        QSignalSpy responses(&client,&MatterClient::cameraMetadataResponse);
        auto payload=guards; payload.insert("name","Never written"); client.invokeCameraMetadata(d,"rename",payload);
        payload=guards; payload.insert("enabled",false); client.invokeCameraMetadata(d,"sync",payload);
        QTRY_COMPARE(responses.size(),2); QCOMPARE(server.commands.size(),0);
        server.intercept=[&](const QJsonObject &r) {
            if (r.value("command")!="device_command") return false;
            if (r.value("args").toObject().value("command_name").toString()!=QStringLiteral("RefreshMetadata")) return false;
            server.attrs=attributes(); auto fresh=snapshot(3); fresh.insert("8","2026-10-24T08:00:00.002Z");
            server.attrs.insert(Prefix+"0",fresh); server.reply(r,QJsonValue(QJsonValue::Null)); return true;
        };
        QVERIFY(manager.refresh(d,baseline));
        QTRY_VERIFY(manager.state(d).confirmed&&!manager.state(d).busy&&manager.state(d).available);
        QCOMPARE(manager.state(d).snapshot.name,QStringLiteral("Cloud camera")); QCOMPARE(server.commands.size(),1);
        QCOMPARE(server.commands.first().value("args").toObject().value("command_name").toString(),QStringLiteral("RefreshMetadata"));
    }
    void malformedIncompleteEnvelopeIsRejectedAtomically() {
        for (int variant=0;variant<4;++variant) {
            Server server; server.attrs=incompleteAttributes(variant!=0);
            if (variant==0) server.attrs.insert(Prefix+"4",true);
            if (variant==1) server.attrs.insert(Prefix+"4",true);
            if (variant==2) { auto value=incompleteSnapshot(true); value.insert("3","residual"); server.attrs.insert(Prefix+"0",value); }
            if (variant==3) server.attrs.insert(Prefix+"1",attributes().value(Prefix+"1"));
            MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected()); CameraMetadataManager manager(&client,QString());
            const auto d=client.devices().first(); QTRY_VERIFY(server.count("read_attribute")==3&&!manager.state(d).busy);
            QVERIFY(!manager.state(d).known); QVERIFY(manager.state(d).stale); QVERIFY(!manager.canRename(d)); QCOMPARE(server.commands.size(),0);
        }
    }
    void cachedIncompleteCannotHydratePreviousMirror() {
        QTemporaryDir directory; Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected()); const auto d=client.devices().first();
        { CameraMetadataManager previous(&client,directory.path()); QTRY_VERIFY(previous.state(d).known&&!previous.state(d).busy); }
        server.attrs=incompleteAttributes(); server.send({{"event","node_updated"},{"data",server.node()}});
        QTRY_VERIFY(client.devices().first().attributes.value(Prefix+"0").toObject().value("8").toString().isEmpty());
        CameraMetadataManager restored(&client,directory.path()); QVERIFY(restored.state(d).snapshot.name.isEmpty()); QVERIFY(restored.state(d).rooms.isEmpty());
        QTRY_VERIFY(restored.state(d).known&&!restored.state(d).busy); QVERIFY(restored.state(d).snapshot.name.isEmpty()); QVERIFY(restored.state(d).rooms.isEmpty());
    }
    void onlyExplicitUnsignedDescriptorDiscoversMetadata() {
        auto node=Server().node(); auto devices=devicesFromNodes({node}); QVERIFY(hasCameraMetadata(devices.first()));
        auto attrs=node.value("attributes").toObject();
        for(const auto &id:QJsonArray{-917502,4294048770.5,"4294048770"}) {
            attrs.insert("1/29/1",QJsonArray{29,0x551,0x553,id}); node.insert("attributes",attrs);
            QVERIFY(!hasCameraMetadata(devicesFromNodes({node}).first()));
        }
    }
    void readsBracketRoomsAndNeverIssueCloudRefresh() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto device=client.devices().first();
        QTRY_VERIFY(manager.state(device).known && !manager.state(device).busy);
        QCOMPARE(server.count("read_attribute"),3); QCOMPARE(server.commands.size(),0);
        QCOMPARE(manager.state(device).rooms.size(),2); QVERIFY(manager.canRename(device));
        const auto paths=[&](int index){return server.requests.at(index+1).value("args").toObject().value("attribute_path").toArray();};
        QCOMPARE(paths(0),QJsonArray{Prefix+"0"}); QCOMPARE(paths(1),QJsonArray({Prefix+"1",Prefix+"2",Prefix+"3",Prefix+"4",Prefix+"5",Prefix+"6"})); QCOMPARE(paths(2),QJsonArray{Prefix+"0"});
    }
    void changingRevisionNeverPublishesMixedRooms() {
        Server server; int reads=0;
        server.intercept=[&](const QJsonObject&r) { if(r.value("command")!="read_attribute")return false; if(++reads==3)server.attrs.insert(Prefix+"0",snapshot(2)); return false; };
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first();
        QTRY_VERIFY(!manager.state(d).busy && reads==3); QVERIFY(!manager.state(d).known); QVERIFY(manager.state(d).conflict);
        QTest::qWait(30); QCOMPARE(reads,3); QCOMPARE(server.commands.size(),0);
    }
    void capabilityAndExpectedRevisionGateEveryWrite() {
        Server server; server.attrs=attributes(false); MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QVERIFY(!manager.rename(d,"New")); QVERIFY(!manager.moveRoom(d,"20")); QVERIFY(!manager.setSync(d,false)); QCOMPARE(server.commands.size(),0);
        auto old=manager.state(d).snapshot; ++old.revision; QVERIFY(!manager.refresh(d,old)); QCOMPARE(server.commands.size(),0); QVERIFY(manager.state(d).conflict);
    }
    void writeTargetsRespectUtf8AndOnlyExistingNonemptyRoom() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QVERIFY(!manager.rename(d,QString(54,QChar(0x4e2d)))); QVERIFY(!manager.moveRoom(d,QString())); QVERIFY(!manager.moveRoom(d,"missing")); QCOMPARE(server.commands.size(),0);
        QVERIFY(manager.rename(d,QString(53,QChar(0x4e2d)))); QTRY_COMPARE(server.commands.size(),1);
    }
    void nullAckNeedsAuthoritativeReadbackAndUsesCamelGuards() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QSignalSpy done(&manager,&CameraMetadataManager::operationFinished); const auto expected=manager.state(d).snapshot;
        QVERIFY(manager.rename(d,"Renamed",expected)); QTRY_COMPARE(server.commands.size(),1);
        const auto request=server.commands.first(); const auto args=request.value("args").toObject();
        QCOMPARE(args.value("cluster_id").toInteger(),4294048770LL); QCOMPARE(args.value("endpoint_id").toInt(),1); QCOMPARE(args.value("command_name").toString(),QStringLiteral("RenameDevice"));
        QCOMPARE(args.value("payload").toObject(),QJsonObject({{"expectedIdentity",expected.identity},{"expectedSourceEpoch",expected.sourceEpoch},{"expectedRevision",1},{"name","Renamed"}}));
        QVERIFY(!manager.rename(d,"Queued")); QCOMPARE(done.size(),0); QVERIFY(!manager.state(d).confirmed);
        auto changed=snapshot(2,"Renamed"); changed.insert("8","2026-10-24T08:00:00.001Z"); server.attrs.insert(Prefix+"0",changed); server.reply(request,QJsonValue(QJsonValue::Null));
        QTRY_COMPARE(done.size(),1); QVERIFY(done.first().at(2).toBool()); QCOMPARE(manager.state(d).snapshot.name,QStringLiteral("Renamed")); QVERIFY(manager.state(d).confirmed);
        QCOMPARE(server.commands.size(),1);
    }
    void idempotentTargetDoesNotRequireRevisionIncrease() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QSignalSpy done(&manager,&CameraMetadataManager::operationFinished);
        QVERIFY(manager.setSync(d,true,manager.state(d).snapshot)); QTRY_COMPARE(server.commands.size(),1); auto fresh=snapshot(); fresh.insert("8","2026-10-24T08:00:00.001Z"); server.attrs.insert(Prefix+"0",fresh); server.reply(server.commands.first(),QJsonValue(QJsonValue::Null));
        QTRY_COMPARE(done.size(),1); QVERIFY(done.first().at(2).toBool()); QCOMPARE(manager.state(d).snapshot.revision,quint64(1));
    }
    void unchangedTargetWithoutFreshObservationCannotConfirm() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QSignalSpy done(&manager,&CameraMetadataManager::operationFinished); QVERIFY(manager.setSync(d,true)); QTRY_COMPARE(server.commands.size(),1);
        server.reply(server.commands.first(),QJsonValue(QJsonValue::Null));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000); QVERIFY(!done.first().at(2).toBool()); QVERIFY(!manager.state(d).confirmed); QCOMPARE(server.commands.size(),1);
    }
    void identityReplacementRejectsLateAcknowledgement() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QSignalSpy done(&manager,&CameraMetadataManager::operationFinished); QVERIFY(manager.rename(d,"Old target")); QTRY_COMPARE(server.commands.size(),1); const auto request=server.commands.first();
        server.report(snapshot(1,"Other camera",QString(64,'c'))); QTRY_VERIFY(done.size()>=1); QVERIFY(!done.first().at(2).toBool());
        server.reply(request,QJsonValue(QJsonValue::Null)); QTRY_VERIFY(manager.state(d).snapshot.identity==QString(64,'c')&&!manager.state(d).busy);
        QCOMPARE(server.commands.size(),1); QCOMPARE(manager.state(d).snapshot.name,QStringLiteral("Other camera"));
    }
    void lateOldIdentityReadCannotOverwriteNewCache() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QJsonObject held;
        server.intercept=[&](const QJsonObject &r) { if (r.value("command")=="read_attribute" && held.isEmpty()) { held=r; return true; } return false; };
        QVERIFY(manager.read(d)); QTRY_VERIFY(!held.isEmpty());
        server.report(snapshot(1,"Replacement",QString(64,'c')));
        QTRY_VERIFY(manager.state(d).snapshot.identity==QString(64,'c')&&!manager.state(d).busy);
        server.reply(held,QJsonObject{{Prefix+"0",snapshot()}}); QTest::qWait(30);
        const auto cache=client.devices().first().attributes.value(Prefix+"0").toObject(); QCOMPARE(cache.value("0").toString(),QString(64,'c'));
        QVERIFY(manager.rename(d,"Valid replacement",manager.state(d).snapshot)); QTRY_COMPARE(server.commands.size(),1);
        QCOMPARE(server.commands.first().value("args").toObject().value("payload").toObject().value("expectedIdentity").toString(),QString(64,'c'));
    }
    void subscriptionUpdatesOnlyReadMetadataAndKeepDraftBaselineStale() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        const auto old=manager.state(d).snapshot; server.report(snapshot(2,"External name"));
        QTRY_VERIFY(manager.state(d).snapshot.revision==2&&!manager.state(d).busy); QCOMPARE(manager.state(d).snapshot.name,QStringLiteral("External name"));
        QVERIFY(!manager.rename(d,"Draft",old)); QCOMPARE(server.commands.size(),0); QCOMPARE(server.count("camera_start_stream"),0); QCOMPARE(server.count("camera_stop_stream"),0);
    }
    void controllerSwitchDiscardsOldPendingAndSameNodeCache() {
        Server first,second; second.attrs.insert(Prefix+"0",snapshot(1,"Controller two",QString(64,'c')));
        MatterClient client; client.connectToServer(first.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto old=client.devices().first(); QTRY_VERIFY(manager.state(old).known&&!manager.state(old).busy);
        QVERIFY(manager.rename(old,"Wrong controller")); QTRY_COMPARE(first.commands.size(),1);
        client.connectToServer(second.url()); QTRY_VERIFY(client.isConnected()); const auto current=client.devices().first();
        QTRY_VERIFY(manager.state(current).known&&!manager.state(current).busy); QCOMPARE(manager.state(current).snapshot.name,QStringLiteral("Controller two"));
        QVERIFY(!manager.rename(old,"Should fail")); QCOMPARE(second.commands.size(),0);
    }
    void directReadNewIdentityStaysCurrentThroughUnrelatedAttributes() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        server.attrs.insert(Prefix+"0",snapshot(2,"Direct newer",QString(64,'c'))); QVERIFY(manager.read(d));
        QTRY_VERIFY(manager.state(d).snapshot.identity==QString(64,'c')&&!manager.state(d).busy); const auto reads=server.count("read_attribute");
        server.send({{"event","attribute_updated"},{"data",QJsonArray{7,"1/6/0",true}}});
        QTest::qWait(40); QCOMPARE(manager.state(d).snapshot.identity,QString(64,'c')); QCOMPARE(server.count("read_attribute"),reads);
    }
    void privateMirrorPersistsOnlyValidatedSnapshotAndRooms() {
        QTemporaryDir directory; Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,directory.path()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        const auto files=QDir(directory.path()).entryList({"*.json"},QDir::Files); QCOMPARE(files.size(),1);
        QFile file(QDir(directory.path()).filePath(files.first())); QVERIFY(file.open(QIODevice::ReadOnly)); const auto root=QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.value("version").toInt(),1); const auto mirror=root.value("mirrors").toObject().value(QString(64,'a')).toObject();
        QCOMPARE(mirror.size(),2); QVERIFY(mirror.contains("snapshot")&&mirror.contains("rooms"));
        QVERIFY(!(file.permissions()&(QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ReadOther|QFileDevice::WriteOther)));
    }
    void timeoutAndMalformedAckNeverRetryWrites() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraMetadataManager manager(&client,QString()); const auto d=client.devices().first(); QTRY_VERIFY(manager.state(d).known&&!manager.state(d).busy);
        QSignalSpy done(&manager,&CameraMetadataManager::operationFinished); QVERIFY(manager.rename(d,"Timeout")); QTRY_COMPARE(server.commands.size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,6500); QVERIFY(!done.first().at(2).toBool()); QCOMPARE(server.commands.size(),1); QVERIFY(!manager.canRename(d));
        server.reply(server.commands.first(),QJsonValue(QJsonValue::Null)); QTest::qWait(30); QCOMPARE(done.size(),1); QCOMPARE(server.commands.size(),1);
    }
};
QTEST_MAIN(CameraMetadataTest)
#include "tst_camerametadata.moc"
