// Local UI review helper. Uses simulated devices only, with caller-isolated XDG data.
#include "mainwindow.h"
#include "devicecard.h"
#include "accessorypanel.h"
#include "homestyle.h"
#include <QApplication>
#include <QDir>
#include <QFont>
#include <QPushButton>
#include <QTimer>
#include <functional>
int main(int argc,char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc,argv); app.setApplicationName("MatterDeskUiPreviews");app.setOrganizationName("MatterDeskTests");
    app.setStyle("Fusion");app.setFont(QFont("Noto Sans CJK SC",10));app.setStyleSheet(homeStyle());
    if(argc!=2) return 2;
    QDir dir(QString::fromLocal8Bit(argv[1]));if(!dir.exists()&&!dir.mkpath("."))return 2;
    MainWindow window;window.show();window.startDemo();bool success=true;
    auto save=[&](QWidget *widget,const QString &name){success=widget->grab().save(dir.filePath(name))&&success;};
    auto page=[&](const QString &key){for(auto *button:window.findChildren<QPushButton *>("navigation"))if(button->property("page").toString()==key&&!button->isHidden()){button->click();return;}};
    QTimer::singleShot(300,&window,[&]{save(&window,"preview.png");page("favorite");});
    QTimer::singleShot(600,&window,[&]{save(&window,"favorites-preview.png");page("scenes");});
    QTimer::singleShot(900,&window,[&]{save(&window,"scenes-preview.png");page("automations");});
    QTimer::singleShot(1200,&window,[&]{save(&window,"automations-preview.png");page("all");for(auto *card:window.findChildren<DeviceCard *>())if(card->device().endpointId==8){card->findChild<QPushButton *>("more")->click();break;}});
    QTimer::singleShot(1550,&window,[&]{if(auto *panel=window.findChild<AccessoryPanel *>()){save(panel,"color-control-preview.png");panel->close();}else success=false;window.resize(940,680);});
    QTimer::singleShot(1850,&window,[&]{save(&window,"compact-preview.png");app.exit(success?0:1);});
    return app.exec();
}
