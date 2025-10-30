%{Cpp:LicenseTemplate}\
%{JS: QtSupport.qtIncludes([], ["QtGui/QGuiApplication", "QtQml/QQmlApplicationEngine"])}

int main(int argc, char *argv[])
{
@if %{UseVirtualKeyboard}
    qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));

@endif
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("%{JS: value('ProjectName')}", "Main");

    return app.exec();
}
