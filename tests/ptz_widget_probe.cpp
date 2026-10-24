#include "matterclient.h"
#include "cameraptzcontrols.h"
#include <QApplication>
#include <QJsonDocument>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>

// Only the isolated synthetic fixture, with explicit opt-in; never a home controller.
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    const auto args=app.arguments();
    if(args.size()!=3 || args.at(1)!="--synthetic-fixture")return 2;
    const QUrl url(args.at(2));
    if(url.scheme()!="ws" || url.host()!="127.0.0.1" || url.port()<1024 || url.port()==5580
       || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())return 2;
    MatterClient client;
    QPointer<CameraPtzControls> controls;
    int direction=0; bool waiting=false;
    QJsonArray accepted;
    QTimer timer;timer.setInterval(20);
    QTimer::singleShot(25000,&app,[&]{app.exit(3);});
    QObject::connect(&client,&MatterClient::connectionChanged,&app,[&]{
        if(controls || !client.isConnected())return;
        for(const auto &device:client.devices()){
            if(!device.available || !device.hasCameraPtz)continue;
            controls=new CameraPtzControls(&client,device);controls->resize(380,170);controls->show();timer.start();break;
        }
    });
    QObject::connect(&timer,&QTimer::timeout,&app,[&]{
        if(!controls || waiting)return;
        static const char *names[]{"ptzUp","ptzDown","ptzLeft","ptzRight"};
        auto *button=controls->findChild<QPushButton*>(names[direction]);
        if(!button || !button->isEnabled())return;
        if(direction==0){const auto preview=qEnvironmentVariable("MATTERDESK_PTZ_PREVIEW");if(!preview.isEmpty())controls->grab().save(preview);}
        waiting=true;button->click();
    });
    QObject::connect(&client,&MatterClient::cameraPtzResponse,&app,
        [&](const QString &,const QString &operation,bool success,const QString &detail){
            if(operation!="move")return;
            if(!success){QTextStream(stderr)<<detail<<'\n';app.exit(4);return;}
            accepted.append(direction++);waiting=false;
            if(direction!=4)return;
            timer.stop();controls->setClosing();
            QTextStream(stdout)<<QJsonDocument(QJsonObject{{"synthetic",true},{"nativeButtons",true},
                {"acceptedDirections",accepted},{"physicalMovementVerified",false}}).toJson(QJsonDocument::Compact)<<'\n';
            controls->deleteLater();client.disconnectFromServer();app.exit(0);
        });
    client.connectToServer(url);
    const int result=app.exec();delete controls;return result;
}
