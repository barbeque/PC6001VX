#include <QtWidgets>
#include <QTimer>
#include <QFileInfo>
#include <QDir>

#include <map>

#include "../event.h"
#include "../osd.h"
#include "../error.h"
#include "../pc6001v.h"
#include "../typedef.h"
#include "../console.h"
#include "../error.h"
#include "../osd.h"
#include "../common.h"
#include "../keyboard.h"

#include "renderview.h"
#include "mainwidget.h"
#include "keypanel.h"
#include "keystatewatcher.h"
#include "p6vxapp.h"

#ifdef Q_OS_ANDROID
#include <QtAndroid>
#include "ekkesShare/shareutils.hpp"
#endif

const QString P6VXApp::keyGeometry				= "window/geometry";
const QString P6VXApp::keyMaximized				= "window/maximized";
const QString P6VXApp::keyHwAccel				= "graph/hwAccel";
const QString P6VXApp::keyFiltering				= "graph/filtering";
const QString P6VXApp::keyFixMagnification		= "graph/fixMagnification";
const QString P6VXApp::keyMagnification			= "graph/magnification";
const QString P6VXApp::keyKeyPanelVisible		= "keypalette/visible";
const QString P6VXApp::keyKeyPanelPosition		= "keypalette/position";
const QString P6VXApp::keyVirtualKeyVisible		= "virtualkey/visible";
const QString P6VXApp::keyVirtualKeyTabIndex	= "virtualkey/tabindex";


///////////////////////////////////////////////////////////
// ROMファイル存在チェック&機種変更
///////////////////////////////////////////////////////////
bool SerchRom( CFG6 *cfg )
{
	char RomSerch[PATH_MAX];

	int IniModel = cfg->GetModel();
	sprintf( RomSerch, "*.%2d", IniModel );
	OSD_AddPath( RomSerch, cfg->GetRomPath(), RomSerch );
	if( OSD_FileExist( RomSerch ) ){
		Error::Reset();
		return true;
	}

	int models[] = { 60, 61, 62, 66, 64, 68 };
	for( int i=0; i < COUNTOF(models); i++ ){
		sprintf( RomSerch, "*.%2d", models[i] );
		OSD_AddPath( RomSerch, cfg->GetRomPath(), RomSerch );
		if( OSD_FileExist( RomSerch ) ){
			cfg->SetModel( models[i] );
			Error::SetError( Error::RomChange );
			return true;
		}
	}
	Error::SetError( Error::NoRom );
	return false;
}

P6VXApp::P6VXApp(int &argc, char **argv)
	: ParentAppClass(argc, argv)
	, P6Core(NULL)
	, Restart(EL6::Quit)
	, Adaptor(new EmulationAdaptor())
	, KPanel(NULL)
	, MouseCursorTimer(new QTimer(this))
	, Setting(QString(OSD_GetModulePath()) + "/pc6001vx.ini", QSettings::IniFormat)
	, TiltEnabled(false)
	, TiltDir(NEWTRAL)
	, TiltStep(0)
	, SafeMode(false)
{
	// アプリで定義した型名をシグナルの引数として使えるようにする
	qRegisterMetaType<HWINDOW>("HWINDOW");
	qRegisterMetaType<TiltDirection>("TiltDirection");
	qRegisterMetaType<FileMode>("FileMode");

	// エミュレーションコア部分用スレッドを生成
	QThread* emulationThread = new QThread(this);
	emulationThread->start();
	Adaptor->moveToThread(emulationThread);

	// ウィンドウを閉じても終了しない。
	// 終了はEL6::Quitが返ってきた時に行う。
	setQuitOnLastWindowClosed (false);

	connect(this, SIGNAL(initialized()), this, SLOT(executeEmulation()), Qt::QueuedConnection);
	connect(this, SIGNAL(vmPrepared()), Adaptor, SLOT(doEventLoop()), Qt::QueuedConnection);
	connect(this, SIGNAL(vmRestart()), this, SLOT(executeEmulation()), Qt::QueuedConnection);
	connect(Adaptor, SIGNAL(finished()), this, SLOT(postExecuteEmulation()), Qt::QueuedConnection);

	// スクリーンセーバー抑止用タイマー
	QTimer* timer = new QTimer(this);
	timer->setInterval(30000);
	connect(timer, SIGNAL(timeout()), this, SLOT(inhibitScreenSaver()));
	timer->start();

	// マウスカーソル表示制御用タイマー
	connect(MouseCursorTimer, &QTimer::timeout, this, &P6VXApp::hideMouseCursor);

}

P6VXApp::~P6VXApp()
{
	Adaptor->thread()->exit();
	Adaptor->thread()->wait();
	Adaptor->deleteLater();
	MWidget->deleteLater();
}

RenderView *P6VXApp::getView()
{
	return MWidget->getMainView();
}

KeyPanel *P6VXApp::getKeyPanel()
{
	return KPanel;
}

VirtualKeyTabWidget* P6VXApp::getVirtualKeyboard()
{
	return MWidget->getVirtualKeyboard();
}



void P6VXApp::startup()
{
#ifndef NOSINGLEAPP
	// 二重起動禁止
	if( isRunning() ) {
		exit();
		return;
	}
#endif

#ifdef Q_OS_ANDROID
	// SDカードへのアクセス許可を要求
	QtAndroid::PermissionResult r = QtAndroid::checkPermission("android.permission.WRITE_EXTERNAL_STORAGE");
	if(r == QtAndroid::PermissionResult::Denied) {
		QtAndroid::requestPermissionsSync( QStringList() << "android.permission.WRITE_EXTERNAL_STORAGE" );
		r = QtAndroid::checkPermission("android.permission.WRITE_EXTERNAL_STORAGE");
		if(r == QtAndroid::PermissionResult::Denied) {
			OSD_Message( QString(tr("Storage access denied.")).toUtf8().constData(), MSERR_ERROR, OSDM_OK | OSDM_ICONERROR );
		}
	}
#endif

	// 設定ファイルパスを作成
	if(!OSD_CreateModulePath()){
		exit();
		return;
	}

	// OSD関連初期化
	if( !OSD_Init() ){
		Error::SetError( Error::InitFailed );
		OSD_Message( (char *)Error::GetErrorText(), MSERR_ERROR, OSDM_OK | OSDM_ICONERROR );
		exit();
		return;
	}

	// コンソール用フォント読込み
	char FontZ[PATH_MAX], FontH[PATH_MAX];
	sprintf( FontZ, ":/res/%s/%s", FONT_DIR, FONTZ_FILE );
	sprintf( FontH, ":/res/%s/%s", FONT_DIR, FONTH_FILE );
	if( !JFont::OpenFont( FontZ, FontH ) ){
		Error::SetError( Error::FontLoadFailed );
		OSD_Message( (char *)Error::GetErrorText(), MSERR_ERROR, OSDM_OK | OSDM_ICONERROR );
		Error::SetError( Error::NoError );
	}

	// P6VXデフォルト設定
#ifndef NOOPENGL
	setDefaultSetting(keyHwAccel, true);
#endif
	setDefaultSetting(keyFiltering, true);
	setDefaultSetting(keyFixMagnification, false);
	setDefaultSetting(keyMagnification, 1.0);
	setDefaultSetting(keyKeyPanelVisible, false);
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
	setDefaultSetting(keyVirtualKeyVisible, true);
#else
	setDefaultSetting(keyVirtualKeyVisible, false);
#endif

	// INIファイル読込み
	if( !Cfg.Init() ){
		switch( Error::GetError() ){
		case Error::IniDefault:
			OSD_Message( (char *)Error::GetErrorText(), MSERR_ERROR, OSDM_OK | OSDM_ICONWARNING );
			Error::SetError( Error::NoError );
			break;

		default:
			OSD_Message( (char *)Error::GetErrorText(), MSERR_ERROR, OSDM_OK | OSDM_ICONERROR );
			exit();
			return;
		}
	}
	overrideSettings(Cfg);

	// ウィンドウ、ウィジェットの生成
	MWidget = new MainWidget();
	KPanel = new KeyPanel(MWidget);

	emit initialized();
}

int P6VXApp::showMessageBox(const char *mes, const char *cap, int type)
{
	QMessageBox::StandardButtons Type = QMessageBox::Ok;
	QMessageBox::Icon IconType = QMessageBox::Information;

	// メッセージボックスのタイプ
	switch( type&0x000f ){
	case OSDM_OK:		Type = QMessageBox::Ok;                         break;
	case OSDM_OKCANCEL:		Type = QMessageBox::Ok | QMessageBox::Cancel;	break;
	case OSDM_YESNO:		Type = QMessageBox::Yes | QMessageBox::No;	break;
	case OSDM_YESNOCANCEL:	Type = QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel;    break;
	}

	// メッセージボックスのアイコンタイプ
	switch( type&0x00f0 ){
	case OSDM_ICONERROR:	IconType = QMessageBox::Critical;	break;
	case OSDM_ICONQUESTION:	IconType = QMessageBox::Question;	break;
	case OSDM_ICONWARNING:	IconType = QMessageBox::Warning;	break;
	case OSDM_ICONINFO:		IconType = QMessageBox::Information;	break;
	}

	QMessageBox mb(IconType, TRANS(cap), TRANS(mes), Type);
	int res = mb.exec();

	switch( res ){
	case QMessageBox::Ok:	return OSDR_OK;
	case QMessageBox::Yes:	return OSDR_YES;
	case QMessageBox::No:	return OSDR_NO;
	default:	return OSDR_CANCEL;
	}
}

const char *P6VXApp::fileDialog(void *hwnd, FileMode mode, const char *title, const char *filter, char *fullpath, char *path, const char *ext)
{
	QSharedPointer<QFileDialog> dialog(createFileDialog(hwnd));
	QString result;
	// 検索パスが指定されていない場合はホームフォルダとする
	QString pathStr = strlen(path) ? QFileInfo(path).dir().path() : QDir::homePath();

	dialog->setWindowTitle(title);
	dialog->setDirectory(pathStr);
	dialog->setNameFilter(filter);

	OSD_ShowCursor(true);
	if(mode == FM_Save){
		dialog->setFileMode(QFileDialog::AnyFile);
		if (dialog->exec() == QDialog::Accepted) {
			result = dialog->selectedFiles().value(0);
		}
		if(result.isEmpty())    return NULL;
		// 入力されたファイル名に拡張子がついていない場合は付与する
		QFileInfo info(result);
		if(info.suffix() != ext){
			result += QString(".") + ext;
		}
		if (OSD_FileExist(result.toUtf8().constData())){
			if (OSD_Message(QString(tr("ファイルはすでに存在しています。上書きしますか?")).toUtf8().constData(),
							NULL, OSDM_OKCANCEL | OSDM_ICONQUESTION)
					== OSDM_OKCANCEL){
				return NULL;
			}
		}
	} else {
		dialog->setFileMode(QFileDialog::ExistingFile);
		if (dialog->exec() == QDialog::Accepted) {
			result = dialog->selectedFiles().value(0);
		}
		if(result.isEmpty()) return NULL;
	}

	QDir dir(result);

	if( path ) strcpy( path, dir.path().toUtf8().constData() );
	if( fullpath ) strcpy( fullpath, result.toUtf8().constData() );
	QFile file(result);
	return file.fileName().toUtf8().constData();
}

const char *P6VXApp::folderDialog(void *hwnd, char *Result)
{
	QSharedPointer<QFileDialog> dialog(createFileDialog(hwnd));
	dialog->setDirectory(strcmp(Result, "/") ? Result : QDir::homePath());
	dialog->setFileMode(QFileDialog::DirectoryOnly);

	QByteArray result;
	OSD_ShowCursor(true);
	if (dialog->exec() == QDialog::Accepted) {
		result = dialog->selectedFiles().value(0).toUtf8();
	}

	strcpy(Result, result);
	return result.constData();
}

void P6VXApp::createWindow(HWINDOW Wh, bool fsflag)
{
	RenderView* view = static_cast<RenderView*>(Wh);
	Q_ASSERT(view);
	QGraphicsScene* scene = view->scene();

#ifdef ALWAYSFULLSCREEN
	MWidget->showFullScreen();
#else
	if (fsflag) {
		MWidget->setWindowState(MWidget->windowState() | Qt::WindowFullScreen);
		MWidget->showFullScreen();
		MWidget->updateLayout();
	} else {
		MWidget->setWindowState(MWidget->windowState() & ~Qt::WindowFullScreen);
		if(!MWidget->isVisible()){
			MWidget->showNormal();
		}
	}
#endif

	view->fitContent();
	OSD_ClearWindow(Wh);

	if(getSetting(keyKeyPanelVisible).toBool()){
		// プラットフォームによっては子ウィンドウをここで作りなおさないと表示されない場合がある
		KPanel->deleteLater();
		KPanel = new KeyPanel(view);
		KPanel->show();
	}
}

void P6VXApp::setWindowIcon(const QIcon &icon)
{
	MWidget->setWindowIcon(icon);
}

void P6VXApp::layoutBitmap(HWINDOW Wh, int x, int y, double scaleX, double scaleY, QImage image)
{
	// QtではSceneRectの幅を返す
	QGraphicsView* view = static_cast<QGraphicsView*>(Wh);
	Q_ASSERT(view);
	QGraphicsScene* scene = view->scene();

	// 指定座標に生成済みのQPixmapItemが存在するかチェック
	// (同一座標にビットマップが重なることはないという前提)
	QGraphicsItem* item = NULL;
	QGraphicsPixmapItem* pItem = NULL;
	foreach(item, scene->items()){
		if(item->scenePos() == QPointF(x, y)){
			// QPixmapItemが見つかったら一旦sceneから除去する
			// (scene上にある状態で画像を更新すると真っ黒になる場合がある)
			pItem = dynamic_cast<QGraphicsPixmapItem*>(item);
			if(pItem) {
				scene->removeItem(pItem);
				break;
			}
		}
	}

	if(pItem == NULL){
		pItem = new QGraphicsPixmapItem(NULL);
		// フィルタリング
		if(getSetting(keyFiltering).toBool()){
			pItem->setTransformationMode(Qt::SmoothTransformation);
		}
	}
	// QPixmapItemの画像を更新してsceneに追加
	pItem->setPixmap(QPixmap::fromImage(image));
	scene->addItem(pItem);

	// アスペクト比に従って縦サイズを調整
	pItem->resetTransform();
	QTransform trans;
	trans.scale(scaleX, scaleY);
	trans.translate(x, y);
	pItem->setTransform(trans);
}

void P6VXApp::getWindowImage(HWINDOW Wh, QRect pos, void **pixels)
{
	QGraphicsView* view = static_cast<QGraphicsView*>(Wh);
	Q_ASSERT(view);
	QGraphicsScene* scene = view->scene();
	QImage image(pos.width(), pos.height(), QImage::Format_RGB32);

	QPainter painter(&image);
	scene->render(&painter, image.rect(), pos);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
	memcpy(*pixels, image.bits(), image.sizeInBytes());
#else
	memcpy(*pixels, image.bits(), image.byteCount());
#endif
}

void P6VXApp::clearLayout(HWINDOW Wh)
{
	QGraphicsView* view = static_cast<QGraphicsView*>(Wh);
	Q_ASSERT(view);
	Q_ASSERT(view->scene());
	QGraphicsScene* scene = view->scene();
	scene->clear();

#ifndef NOOPENGL
	// ステータスバー非表示またはフルスクリーン、かつTILTモードが有効になっている場合、背景を描く
	if( (!Cfg.GetDispStat() || Cfg.GetFullScreen()) &&
		#ifndef NOMONITOR
			!Cfg.GetMonDisp() &&
		#endif
			isTiltEnabled()){
		// 画面に対する、枠を含めたサイズの比率
		qreal merginRatio = 1.0;
		QGraphicsPixmapItem* background = NULL;
		switch(this->Cfg.GetModel()){
		case 60:
		case 61:
			// 初代機の場合はPC-6042Kを使う
			background = new QGraphicsPixmapItem(QPixmap::fromImage(QImage(":/res/background60.png")));
			merginRatio = 1.45;
			break;
		default:
			// それ以外の場合はPC-60m43を使う
			background = new QGraphicsPixmapItem(QPixmap::fromImage(QImage(":/res/background.png")));
			merginRatio = 1.2;
		}
		background->setTransformationMode(Qt::SmoothTransformation);
		// 最前面に配置(他のアイテムのZ値はデフォルトの0)
		background->setZValue(1);
		QTransform trans;
		// 画像の拡大倍率
		qreal ratio = qMax(scene->width() / background->sceneBoundingRect().width(),
						   scene->height() / background->sceneBoundingRect().height());
		qreal scaleRatio = ratio * merginRatio;

		int scaledWidth = background->sceneBoundingRect().width() * scaleRatio;
		int scaledHeight = background->sceneBoundingRect().height() * scaleRatio;

		// 画像のオフセットを計算(枠の分だけ左上に移動)
		trans.translate(-(scaledWidth - scene->width()) / 2, -(scaledHeight - scene->height()) / 2);
		trans.scale(ratio * merginRatio, ratio * merginRatio);
		background->setTransform(trans);
		scene->addItem(background);
	}
#endif
}

void P6VXApp::showPopupMenu(int x, int y)
{
	// メニュー表示中に右クリックすると二重にメニューが表示されてしまうため、その対処
	if(MenuMutex.tryLock()){
		P6Core->Stop();
		P6Core->ShowPopupImpl(x, y);
		P6Core->Start();
		MenuMutex.unlock();
	}
}

void P6VXApp::toggleKeyPanel()
{
	if(KPanel->isVisible()){
		KPanel->close();
	} else {
		KPanel->show();
	}
}

void P6VXApp::toggleVirtualKeyboard()
{
	MWidget->toggleVirtualKeyboard();
}

void P6VXApp::activateMouseCursorTimer()
{
	MouseCursorTimer->start(5000);
}

void P6VXApp::deactivateMouseCursorTimer()
{
	MouseCursorTimer->stop();
}

void P6VXApp::resetSettings()
{
	if (OSD_Message(QString(tr("本当に設定を初期化しますか?")).toUtf8().constData(),
					NULL, OSDM_YESNO | OSDM_ICONWARNING) == OSDR_YES){
		if(OSD_Message(QtEL6::tr("設定を反映するには一度終了しますがよろしいですか?").toUtf8().constData(),
					   MSG_QUITC, OSDM_YESNO | OSDM_ICONWARNING) == OSDR_YES){

			char path[PATH_MAX];
#ifdef AUTOSUSPEND
			// 自動ステートセーブファイルを削除
			OSD_AddPath( path, Cfg.GetDokoSavePath(), ".0.dds" );
			QFile::remove(path);
#endif
			// P6V設定ファイルを削除
			OSD_AddPath( path, OSD_GetModulePath(), CONF_FILE );
			QFile::remove(path);
			// P6VX設定ファイルを削除
			OSD_AddPath( path, OSD_GetModulePath(), "pc6001vx.ini" );
			QFile::remove(path);
			// アプリを終了(終了時に設定を保存しないようにしてから)
			this->Cfg.SetSaveQuit(false);
			OSD_PushEvent(EV_QUIT);
		}
	}
}

bool P6VXApp::isTiltEnabled()
{
	QMutexLocker lock(&PropretyMutex);
	return TiltEnabled;
}

void P6VXApp::enableTilt(bool enable)
{
	QMutexLocker lock(&PropretyMutex);
	TiltEnabled = enable;
}

TiltDirection P6VXApp::getTiltDirection()
{
	QMutexLocker lock(&PropretyMutex);
	return TiltDir;
}

void P6VXApp::setTiltDirection(TiltDirection dir)
{
	QMutexLocker lock(&PropretyMutex);
	TiltDir = dir;
}

void P6VXApp::setTiltStep(int step)
{
	QMutexLocker lock(&PropretyMutex);
	TiltStep = step;
}

bool P6VXApp::isPlatform(const QString platform)
{
	return platformName() == platform;
}

bool P6VXApp::isAVI()
{
	return P6Core->IsAVI();
}

int P6VXApp::getTiltStep()
{
	QMutexLocker lock(&PropretyMutex);
	return TiltStep;
}

bool P6VXApp::isSafeMode()
{
	QMutexLocker lock(&PropretyMutex);
	return SafeMode;
}

void P6VXApp::enableSafeMode(bool enable)
{
	QMutexLocker lock(&PropretyMutex);
	SafeMode = enable;
}

QString P6VXApp::getCustomRomPath()
{
	QMutexLocker lock(&PropretyMutex);
	return CustomRomPath;
}

void P6VXApp::setCustomRomPath(QString path)
{
	QMutexLocker lock(&PropretyMutex);
	CustomRomPath = path;
}

void P6VXApp::enableCompatibleRomMode(CFG6* config, bool enable)
{
	QMutexLocker lock(&PropretyMutex);
	// P6V本体の設定ファイルに対して互換ROMに必要な設定を行う。
	// この時点では設定ファイルに反映しない点に注意。
	if(enable){
		// 互換ROM使用時の設定
		auto model = config->GetModel();
		if (model != 60 && model != 62 && model != 66){
			config->SetModel(60);
		}
		config->SetCheckCRC(false);
		config->SetRomPath(":/res/rom");
	} else {
		config->SetCheckCRC(true);
		config->SetRomPath("");
	}
}

void P6VXApp::exportSavedTape()
{
	// TAPE(SAVE)ファイル名を取得
	auto src = Cfg.GetSaveFile();
	if (!OSD_FileExist(src)){
		OSD_Message(QString(tr("TAPE(SAVE)ファイルが存在しません。")).toUtf8().constData(), MSERR_ERROR, OSDM_OK | OSDM_ICONERROR);
		return;
	}
#ifdef Q_OS_ANDROID
	// Androidの場合はインテントで他のアプリに送る
	ShareUtils util;
	int req = 0;
	bool altImpl = false;
	util.sendFile(QDir::cleanPath(src), "TAPE(TAPE)", "application/octet-stream", req, altImpl);
#else
	// エクスポート先を指定
	char dest[PATH_MAX];
	strcpy(dest, QDir::homePath().toLocal8Bit());
	QFile savedTape(src);
	if(OSD_FileSelect( nullptr, FD_TapeSave, dest, src )){
		if (OSD_FileExist(dest)){
			if (OSD_Message(QString(tr("ファイルはすでに存在しています。上書きしますか?")).toUtf8().constData(),
							NULL, OSDM_OKCANCEL | OSDM_ICONQUESTION)
					== OSDR_OK){
				CFG6 cfg;
				cfg.Write();
			}
		}
		savedTape.rename(dest);
	}
#endif

}

//仮想マシンを開始させる
void P6VXApp::executeEmulation()
{
	// カスタムROMパスが設定されている場合はそちらを使う
	if(getCustomRomPath() != ""){
		Cfg.SetRomPath(getCustomRomPath().toUtf8().data());
	}

	// ROMファイル存在チェック&機種変更
	if( SerchRom( &Cfg ) ){
		if( Error::GetError() != Error::NoError ){
			OSD_Message( (char *)Error::GetErrorText(), MSERR_ERROR, OSDM_OK | OSDM_ICONWARNING );
			Error::SetError( Error::NoError );
		}
	}else{
		bool romFolderSpecified = false;
		if(OSD_Message( QString(tr("ROMファイルが見つかりません。\n"
								   "ROMフォルダ(%1)にROMファイルをコピーするか、"
								   "別のROMフォルダを指定してください。\n"
								   "別のROMフォルダを指定しますか?")).arg(Cfg.GetRomPath()).toUtf8().constData(), MSERR_ERROR, OSDM_YESNO | OSDM_ICONWARNING ) == OSDR_YES){
			// ROMフォルダ再設定
			char folder[PATH_MAX];
			strncpy(folder, Cfg.GetRomPath(), PATH_MAX);
			OSD_AddDelimiter(folder);
			OSD_FolderDiaog(MWidget, folder);
			OSD_DelDelimiter(folder);

			if(strlen(folder) > 0){
				Cfg.SetRomPath(folder);
				Cfg.Write();
				Restart = EL6::Restart;
				romFolderSpecified = true;
			}
		}
		if (!romFolderSpecified){
			// 互換ROMを使用するか問い合わせる
			int ret = OSD_Message( tr("エミュレーター内蔵の互換ROMを使用しますか?").toUtf8().constData(),
								   NULL, OSDM_YESNO | OSDM_ICONQUESTION );
			if(ret == OSDR_YES) {
				enableCompatibleRomMode(&Cfg, true);
				Cfg.Write();
				Restart = EL6::Restart;
			} else {
				terminateEmulation();
				exit();
				return;
			}
		}
		emit vmRestart();
		return;
	}

	// 機種別P6オブジェクト確保
	QScopedPointer<QtEL6> P6CoreObj(new QtEL6(this));
	if( !P6CoreObj ){
		exit();
		return;
	}

	// VM初期化
	if( !P6CoreObj->Init( &Cfg ) ){
		if(Error::GetError() == Error::RomCrcNG){
			// CRCが合わない場合
			int ret = OSD_Message( tr("ROMイメージのCRCが不正です。\n"
									  "CRCが一致しないROMを使用すると、予期せぬ不具合を引き起こす可能性があります。\n"
									  "それでも起動しますか?").toUtf8().constData(),
								   MSERR_ERROR, OSDM_YESNO | OSDM_ICONWARNING );
			if(ret == OSDR_YES) {
				Cfg.SetCheckCRC(false);
				Cfg.Write();
				emit vmRestart();
				return;
			} else {
				terminateEmulation();
				exit();
				return;
			}
		}else if(Error::GetError() == Error::NoRom){
			// ROMの一部が見つからない場合
			int ret = OSD_Message( tr("ROMファイルの一部が見つかりません。\n"
									  "エミュレーター内蔵の互換ROMを使用しますか?").toUtf8().constData(),
								   MSERR_ERROR, OSDM_YESNO | OSDM_ICONWARNING );
			if(ret == OSDR_YES) {
				enableCompatibleRomMode(&Cfg, true);
				Cfg.Write();
				emit vmRestart();
				return;
			} else {
				terminateEmulation();
				exit();
				return;
			}
		}else{
			OSD_Message( (char *)Error::GetErrorText(), MSERR_ERROR, OSDM_OK | OSDM_ICONERROR );
			exit();
			return;
		}
	}

	switch( Restart ){
	case EL6::Quit:	// 通常起動
#ifdef AUTOSUSPEND
		// 自動サスペンド有効時はここでLOAD
		P6CoreObj->DokoDemoLoad(0);
#endif
		break;
	case EL6::Dokoload:	// どこでもLOAD?
		P6CoreObj->DokoDemoLoad( Cfg.GetDokoFile() );
		break;

	case EL6::Replay:	// リプレイ再生?
	{
		char temp[PATH_MAX];
		strncpy( temp, Cfg.GetDokoFile(), PATH_MAX );
		P6CoreObj->DokoDemoLoad( temp );
		P6CoreObj->REPLAY::StartReplay( temp );
	}
		break;

	default:
		break;
	}

	P6Core = P6CoreObj.take();

	// キーボード状態監視
	KeyWatcher = new KeyStateWatcher(P6Core->GetKeyboard(), this);
	MWidget->setKeyStateWatcher(KeyWatcher);

	// 以降、ウィンドウが閉じたらアプリを終了する
	connect(this, SIGNAL(lastWindowClosed()), this, SLOT(terminateEmulation()));

	// VM実行
	Adaptor->setEmulationObj(P6Core);
	emit vmPrepared();
	P6Core->Start();
}

//仮想マシン終了後の処理
void P6VXApp::postExecuteEmulation()
{
	Restart = Adaptor->getReturnCode();
	Adaptor->setEmulationObj(NULL);

	if(P6Core){
		P6Core->Stop();

#ifdef AUTOSUSPEND
		// 自動サスペンド有効時はここでSAVE
		if( Restart == EL6::Quit ){
			P6Core->DokoDemoSave(0);
		}
#endif
		P6Core->deleteLater();
	}

	// 再起動ならばINIファイル再読込み
	if( Restart == EL6::Restart ){
		if( !Cfg.Init() ){
			Error::SetError( Error::IniDefault );
			OSD_Message( (char *)Error::GetErrorText(), MSERR_ERROR, OSDM_OK | OSDM_ICONWARNING );
			Error::SetError( Error::NoError );
		}
	}

	if( Restart == EL6::Quit ){
		// 終了処理
		if(MWidget && MWidget->isVisible()){
			// ウィンドウを閉じてサイズを保存
			MWidget->close();
		}
		exit();
		return;
	} else {
		emit vmRestart();
	}
}

void P6VXApp::terminateEmulation()
{
	OSD_PushEvent( EV_QUIT );
}



void P6VXApp::handleSpecialKeys(QKeyEvent* ke, int& keyCode)
{
	quint32 nativeKey = ke->nativeScanCode();
#if 0
	qDebug("keytext %s\n", ke->text().toStdString().c_str());
	qDebug("keycode 0x%x\n", keyCode);
	qDebug("nativekeycode %d\n", nativeKey);
#endif

	// X11の場合
	if (isPlatform("xcb")){
		// 「ろ」
		if(keyCode == Qt::Key_Backslash){
			keyCode = nativeKey == 97 ? Qt::Key_Underscore : Qt::Key_Backslash;
		}
	}
	// Windowsの場合
	else if (isPlatform("windows")){
		// 「ろ」
		if(keyCode == Qt::Key_Backslash){
			keyCode = nativeKey == 115 ? Qt::Key_Underscore : Qt::Key_Backslash;
		}
		// 変換
		else if(nativeKey == 121){
			keyCode = Qt::Key_Henkan;
		}
		// 無変換
		else if(nativeKey == 123){
			keyCode = Qt::Key_Muhenkan;
		}
	}
}

void P6VXApp::finishKeyEvent(Event &ev)
{
	// qDebug() << ev.key.sym << QChar(ev.key.unicode);

	// かな、グラフィックキー有効時はキーイベントをそのまま送信
	auto mod = KeyWatcher->GetModifierStatus();
	if ( mod & KeyStateWatcher::KANA ||
		 mod & KeyStateWatcher::KKANA ||
		 mod & KeyStateWatcher::GRAPH ||
		 mod & KeyStateWatcher::CAPS ||
		 mod & KeyStateWatcher::KANA) {
		OSD_PushEvent(ev);
		return;
	}

	// イベント時点でのシフトキー状態
	bool shiftStatus = ev.key.mod & KVM_SHIFT;

	struct FixInfo
	{
		PCKEYsym sym;
		bool shift;
	};
#define QUNI(a) QChar(a).unicode()
	static const std::map<uint16_t, FixInfo> fixMap = {
		{	QUNI('-'),	{ KVC_MINUS,		false}	},
		{	QUNI('^'),	{ KVC_CARET,		false}	},
		{	QUNI('@'),	{ KVC_AT,			false}	},
		{	QUNI('['),	{ KVC_LBRACKET,		false}	},
		{	QUNI(']'),	{ KVC_RBRACKET,		false}	},
		{	QUNI(';'),	{ KVC_SEMICOLON,	false}	},
		{	QUNI(':'),	{ KVC_COLON,		false}	},
		{	QUNI(','),	{ KVC_COMMA,		false}	},
		{	QUNI('.'),	{ KVC_PERIOD,		false}	},
		{	QUNI('/'),	{ KVC_SLASH,		false}	},
		{	QUNI('!'),	{ KVC_1,			true}	},
		{	QUNI('\"'),	{ KVC_2,			true}	},
		{	QUNI('#'),	{ KVC_3,			true}	},
		{	QUNI('$'),	{ KVC_4,			true}	},
		{	QUNI('%'),	{ KVC_5,			true}	},
		{	QUNI('&'),	{ KVC_6,			true}	},
		{	QUNI('\''),	{ KVC_7,			true}	},
		{	QUNI('('),	{ KVC_8,			true}	},
		{	QUNI(')'),	{ KVC_9,			true}	},
		{	QUNI('='),	{ KVC_MINUS,		true}	},
		{	QUNI('+'),	{ KVC_SEMICOLON,	true}	},
		{	QUNI('*'),	{ KVC_COLON,		true}	},
		{	QUNI('_'),	{ KVC_UNDERSCORE,	true}	},
		{	QUNI('<'),	{ KVC_COMMA,		true}	},
		{	QUNI('>'),	{ KVC_PERIOD,		true}	},
		{	QUNI('?'),	{ KVC_SLASH,		true}	},
	};
#undef QUNI
	static const Event shiftDownEvent =
	{
		.key = { EV_KEYDOWN, true, KVC_LSHIFT, KVM_LSHIFT, 0 }
	};
	static const Event shiftUpEvent =
	{
		.key = { EV_KEYUP, false, KVC_LSHIFT, KVM_LSHIFT, 0 }
	};

	// 英語キーボードの場合、@の入力にSHIFTキーが必要なため、
	// この場合P6エミュ側にSHIFT+@のキーコードを送っても何も入力されない。
	// 本来意図した文字が入力されるよう、
	// キーシンボルとモディファイヤキーを修正する。
	if (fixMap.count(ev.key.unicode)){
		auto i = fixMap.at(ev.key.unicode);
		ev.key.sym = i.sym;
		if (i.shift){
			// P6でSHIFTが必要な記号はSHIFTモディファイヤーを付与する
			OSD_PushEvent(shiftDownEvent);	// SHIFTキーを押すイベントを挿入する
			// キーイベント本体にSHIFTモディファイヤーを付与
			ev.key.mod = (PCKEYmod)(ev.key.mod|KVM_SHIFT);
			// キーイベントを送信
			OSD_PushEvent(ev);
			// キーを離した後、物理SHIFTキーの状態が離されていたならば
			// SHIFTキーを離すイベントをキューに入れる(キーリピート中に離さないようにイベント優先度を下げる)
			if(!ev.key.state && !shiftStatus) {
				postEvent(this, new QKeyEvent(QEvent::KeyRelease, Qt::Key_Shift,
											  Qt::NoModifier), Qt::LowEventPriority);
			}
		} else {
			// P6でSHIFTなしで入力できる記号はSHIFTモディファイヤーを外す
			// SHIFTキーを離すイベントを挿入する
			OSD_PushEvent(shiftUpEvent);
			// キーイベント本体にSHIFTモディファイヤーを付与
			ev.key.mod = (PCKEYmod)(ev.key.mod&~KVM_SHIFT);
			// キーイベントを送信
			OSD_PushEvent(ev);
			// キーを離した後、物理SHIFTキーの状態が押されていたならば
			// SHIFTキーを押すイベントをキューに入れる(キーリピート中に押さないようにイベント優先度を下げる)
			if(!ev.key.state && shiftStatus) {
				postEvent(this, new QKeyEvent(QEvent::KeyPress, Qt::Key_Shift,
											  Qt::NoModifier), Qt::LowEventPriority);
			}
		}
	} else {
		// 修正対象に該当しなかったキーイベントはそのまま送信
		OSD_PushEvent(ev);
	}
}

void P6VXApp::overrideSettings(CFG6 &cfg)
{
#ifdef Q_OS_ANDROID
	char str[PATH_MAX];
	// 設定のデフォルト値でなく、プラットフォームごとのデータフォルダを探す。
	// Androidではこの方法でないとSDカードが検出できない場合がある。
	const auto dataPath = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)[0].toLocal8Bit();
	// ROM,TAPE,DISK,拡張ROM,WAVEパスの初期値にはこのデータフォルダを設定する。
	if(QString(cfg.GetRomPath()).contains(OSD_GetModulePath())) cfg.SetRomPath(dataPath);
	if(QString(cfg.GetTapePath()).contains(OSD_GetModulePath())) cfg.SetTapePath(dataPath);
	if(QString(cfg.GetDiskPath()).contains(OSD_GetModulePath())) cfg.SetDiskPath(dataPath);
	if(QString(cfg.GetExtRomPath()).contains(OSD_GetModulePath())) cfg.SetExtRomPath(dataPath);
	if(QString(cfg.GetWavePath()).contains(OSD_GetModulePath())) cfg.SetWavePath(dataPath);

	// AndroidではSDカードにファイルを書き出せないため、
	// SnapshotおよびどこでもSAVEはアプリ内のサンドボックス領域に固定する。
	OSD_AddPath( str, OSD_GetModulePath(), IMAGE_DIR );
	cfg.SetImgPath( str );
	OSD_AddPath( str, OSD_GetModulePath(), DOKOSAVE_DIR );
	cfg.SetDokoSavePath( str );
	cfg.Write();

	// Androidでは起動時にスナップショット画像を削除する
	auto imgDir = QDir(cfg.GetImgPath());
	imgDir.setNameFilters(QStringList() << "*.png");
	imgDir.setFilter(QDir::Files);
	foreach(QString dirFile, imgDir.entryList())
	{
		imgDir.remove(dirFile);
	}
#endif

	// サンプリングレートは44100に固定
	cfg.SetSampleRate(44100);
}

QFileDialog *P6VXApp::createFileDialog(void *hwnd)
{
	QWidget* parent = static_cast<QWidget*>(hwnd);
	auto dialog = new QFileDialog(parent);
	// GTKスタイル使用時にファイル選択ダイアログがフリーズする対策
	// Androidのネイティブファイルダイアログが動かないための暫定措置
	// https://bugreports.qt.io/browse/QTBUG-77214
	QFileDialog::Options opt = QFileDialog::ShowDirsOnly;
	if (isPlatform("xcb") || isPlatform("android")){
		opt |= QFileDialog::DontUseNativeDialog;
	}
	dialog->setOptions(opt);
#ifdef ALWAYSFULLSCREEN
	dialog->setWindowState(dialog->windowState() | Qt::WindowFullScreen);
#endif
#ifdef Q_OS_ANDROID
	// シングルタップで開くように設定
	dialog->setStyleSheet(QStringLiteral("QAbstractItemView { activate-on-singleclick: 1; }"));
	// Androidではローカルストレージのトップフォルダをブックマークさせる。
	// こうしないとなぜかファイル選択ダイアログにローカルストレージが表示されない。
	QStringList dataPaths;
	dataPaths << "/storage";
	dataPaths << QProcessEnvironment::systemEnvironment().value("EXTERNAL_STORAGE");
	dataPaths += QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
	auto sidebarList = dialog->sidebarUrls();
	foreach (auto path, dataPaths){
		auto uri = QUrl::fromLocalFile(path);
		if (!sidebarList.contains(uri) && QFile::exists(path)){
			sidebarList << uri;
		}
	}
	dialog->setSidebarUrls(sidebarList);
#endif
	return dialog;
}

bool P6VXApp::notify ( QObject * receiver, QEvent * event )
{
	Event ev;
	ev.type = EV_NOEVENT;

	switch(event->type()){
	case QEvent::KeyPress:
	case QEvent::KeyRelease:
	{
		QKeyEvent* ke = dynamic_cast<QKeyEvent*>(event);
		Q_ASSERT(ke);

		// キーコードを特定
		int keyCode = Qt::Key_unknown;
#ifdef IRREGULAR_KEYBOARD
		// UMPC内蔵小型キーボードなど、特殊なキーボードの場合
		// 一般的なキーボードとコードのアンマッチがあるので
		// 表示可能文字についてはtextを参照したほうが信頼できる
		if(!ke->text().isEmpty() && ke->text().at(0).isPrint()){
			keyCode = ke->text().at(0).toUpper().unicode();
		} else {
			keyCode = ke->key();
		}
#else
		keyCode = ke->key();
#endif
		// 特殊キー対策
		handleSpecialKeys(ke, keyCode);

		// 以下の場合は入力イベントをエミュレータ側に渡さず、Qtに渡す
		// (メニューやダイアログでキーボードを使えるようにするため)
		bool processKeyEventInQt = false;
		// ・エミュレーションポーズ中
		// 　例外としてF9キー(ポーズ解除)とF12(スナップショット)は
		// 　エミュレータで受け付ける
		if(!(P6Core && !P6Core->IsCancel() && !P6Core->GetPauseEnable())
				&& keyCode != Qt::Key_F9
				&& keyCode != Qt::Key_F12){
			processKeyEventInQt = true;
		}
		// ・エミュレータウィンドウ、キーパレット以外のウィンドウ
		// (Aboutダイアログ、環境設定ダイアログ)が最前面にある場合
		QString activeWindowClass = activeWindow() ? activeWindow()->metaObject()->className() : "";
		if(activeWindowClass != "MainWidget" && activeWindowClass != "KeyPanel"){
			processKeyEventInQt = true;
		}
		// メニュー表示中の場合
		if(!MenuMutex.tryLock()){
			processKeyEventInQt = true;
		} else {
			MenuMutex.unlock();
		}
		if(processKeyEventInQt){
			return ParentAppClass::notify(receiver, event);
		}


		ev.type        = event->type() == QEvent::KeyPress ? EV_KEYDOWN : EV_KEYUP;
		ev.key.state   = event->type() == QEvent::KeyPress ? true : false;
		ev.key.sym     = OSD_ConvertKeyCode( keyCode );
		ev.key.mod	   = (PCKEYmod)(
					( ke->modifiers() & Qt::ShiftModifier ? KVM_SHIFT : KVM_NONE )
					| ( ke->modifiers() & Qt::ControlModifier ? KVM_CTRL : KVM_NONE )
					| ( ke->modifiers() & Qt::AltModifier ? KVM_ALT : KVM_NONE )
					| ( ke->modifiers() & Qt::MetaModifier ? KVM_CTRL : KVM_NONE )
					| ( ke->modifiers() & Qt::KeypadModifier ? KVM_NUM : KVM_NONE )
					// CAPSLOCKは検出できない？
					// | ( ke->modifiers() & Qt::caps ? KVM_NUM : KVM_NONE )
					);
		ev.key.unicode = ke->text().utf16()[0];
		finishKeyEvent(ev);
		break;
	}
#ifdef ALWAYSFULLSCREEN
	case QEvent::ApplicationStateChange:
		if(P6Core){
			if(static_cast<QApplicationStateChangeEvent*>(event)->applicationState()){
				P6Core->Start();
			}else{
				P6Core->Stop();
#ifdef AUTOSUSPEND
				// 自動サスペンド有効時はここでSAVE
				P6Core->DokoDemoSave(0);
#endif
			}
		}
		break;
#endif
	default:;
	}

	if(ev.type == EV_NOEVENT){
		return ParentAppClass::notify(receiver, event);
	} else {
		return true;
	}
}

bool P6VXApp::hasSetting(const QString &key)
{
	QMutexLocker lock(&SettingMutex);
	return Setting.contains(key);
}

const QVariant P6VXApp::getSetting(const QString &key)
{
	QMutexLocker lock(&SettingMutex);
	return Setting.value(key);
}

void P6VXApp::setSetting(const QString &key, const QVariant &value)
{
	QMutexLocker lock(&SettingMutex);
	if(Setting.value(key) != value){
		Setting.setValue(key, value);
	}
}

void P6VXApp::setDefaultSetting(const QString &key, const QVariant &value)
{
	QMutexLocker lock(&SettingMutex);
	if(!Setting.contains(key)){
		Setting.setValue(key, value);
	}
}

#ifdef USE_X11
#include <QX11Info>
#include <X11/Xlib.h>
#endif
void P6VXApp::inhibitScreenSaver()
{
#if defined USE_X11
	if(MWidget && MWidget->isFullScreen()){
		XResetScreenSaver(QX11Info::display());
	}
#elif defined Q_OS_WIN
	if(MWidget && MWidget->isFullScreen()){
		SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
	} else {
		SetThreadExecutionState(ES_CONTINUOUS);
	}
#else
	// 何もしない
#endif
}

void P6VXApp::hideMouseCursor()
{
	setOverrideCursor(Qt::BlankCursor);
}

