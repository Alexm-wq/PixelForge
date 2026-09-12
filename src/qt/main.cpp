#include "Editor.hpp"
#include "LocalMcpBridge.hpp"
#include <QCoreApplication>
#include <QApplication>
#include <QMessageBox>
int main(int argc,char** argv) {
    if(argc==4&&QString::fromLocal8Bit(argv[1])=="--mcp"&&QString::fromLocal8Bit(argv[2])=="--socket") {
        QCoreApplication app(argc,argv);return pixelforge::qt::LocalMcpBridge::forwardStdio(QString::fromLocal8Bit(argv[3]));
    }
    QApplication app(argc,argv);
    app.setApplicationName("PixelForge Qt"); app.setOrganizationName("PixelForge");
    app.setStyle("Fusion");
    pixelforge::qt::Editor editor; editor.show();
    if(argc>1) {QString error; if(!editor.openFile(QString::fromLocal8Bit(argv[1]),&error)) QMessageBox::warning(&editor,"Open failed",error);}
    return app.exec();
}
