/*
 * Hydrogen
 * Copyright(c) 2002-2008 by Alex >Comix< Cominu [comix@users.sourceforge.net]
 *
 * http://www.hydrogen-music.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <hydrogen/event_queue.h>
#include <hydrogen/version.h>
#include <hydrogen/hydrogen.h>
#include <hydrogen/playlist.h>
#include <hydrogen/audio_engine.h>
#include <hydrogen/smf/SMF.h>
#include <hydrogen/Preferences.h>
#include <hydrogen/timeline.h>
#include <hydrogen/LocalFileMng.h>
#include <hydrogen/basics/pattern.h>
#include <hydrogen/basics/pattern_list.h>
#include <hydrogen/basics/instrument_list.h>
#include <hydrogen/basics/instrument_layer.h>


#include "AboutDialog.h"
#include "AudioEngineInfoForm.h"
#include "ExportSongDialog.h"
#include "HydrogenApp.h"
#include "InstrumentRack.h"
#include "Skin.h"
#include "MainForm.h"
#include "PlayerControl.h"
#include "HelpBrowser.h"
#include "LadspaFXProperties.h"
#include "SongPropertiesDialog.h"
#include "UndoActions.h"

#include "Director.h"
#include "Mixer/Mixer.h"
#include "InstrumentEditor/InstrumentEditorPanel.h"
#include "PatternEditor/PatternEditorPanel.h"
#include "SongEditor/SongEditor.h"
#include "SongEditor/SongEditorPanel.h"
#include "SoundLibrary/SoundLibraryPanel.h"
#include "SoundLibrary/SoundLibraryImportDialog.h"
#include "SoundLibrary/SoundLibrarySaveDialog.h"
#include "SoundLibrary/SoundLibraryExportDialog.h"
#include "PlaylistEditor/PlaylistDialog.h"

#include <QtGui>

#ifndef WIN32
#include <sys/time.h>
#include <sys/socket.h>
#endif

#ifdef H2CORE_HAVE_LASH
#include <lash-1.0/lash/lash.h>
#include <hydrogen/LashClient.h>
#endif

#include <memory>
#include <cassert>

using namespace std;
using namespace H2Core;

int MainForm::sigusr1Fd[2];

const char* MainForm::__class_name = "MainForm";

MainForm::MainForm( QApplication *app, const QString& songFilename )
	: QMainWindow( 0, 0 )
	, Object( __class_name )
{
	setMinimumSize( QSize( 1000, 500 ) );
	setWindowIcon( QPixmap( Skin::getImagePath() + "/icon16.png" ) );

#ifndef WIN32
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigusr1Fd))
		qFatal("Couldn't create HUP socketpair");
	snUsr1 = new QSocketNotifier(sigusr1Fd[1], QSocketNotifier::Read, this);
	connect(snUsr1, SIGNAL(activated(int)), this, SLOT( handleSigUsr1() ));
#endif


	m_pQApp = app;

	m_pQApp->processEvents();

	// Load default song
	Song *pSong = NULL;

	if ( !songFilename.isEmpty() ) {
		pSong = Song::load( songFilename );

		/*
		 * If the song could not be loaded, create
		 * a new one with the specified filename
		 */
		if (pSong == NULL) {
			pSong = Song::get_empty_song();
			pSong->set_filename( songFilename );
		}
	}
	else {
		Preferences *pref = Preferences::get_instance();
		bool restoreLastSong = pref->isRestoreLastSongEnabled();
		QString filename = pref->getLastSongFilename();
		if ( restoreLastSong && ( !filename.isEmpty() )) {
			pSong = Song::load( filename );
			if (pSong == NULL) {
				//QMessageBox::warning( this, "Hydrogen", trUtf8("Error restoring last song.") );
				pSong = Song::get_empty_song();
				pSong->set_filename( "" );
			}
		}
		else {
			pSong = Song::get_empty_song();
			pSong->set_filename( "" );
		}
	}

	h2app = new HydrogenApp( this, pSong );
	h2app->addEventListener( this );
	createMenuBar();

	h2app->setStatusBarMessage( trUtf8("Hydrogen Ready."), 10000 );

	initKeyInstMap();

	// we need to do all this to support the keyboard playing
	// for all the window modes
	h2app->getMixer()->installEventFilter (this);
	h2app->getPatternEditorPanel()->installEventFilter (this);
	h2app->getPatternEditorPanel()->getPianoRollEditor()->installEventFilter (this);
	h2app->getSongEditorPanel()->installEventFilter (this);
	h2app->getPlayerControl()->installEventFilter(this);
	InstrumentEditorPanel::get_instance()->installEventFilter(this);
	h2app->getAudioEngineInfoForm()->installEventFilter(this);
	h2app->getDirector()->installEventFilter(this);
	//	h2app->getPlayListDialog()->installEventFilter(this);
	installEventFilter( this );

	showDevelWarning();

	connect( &m_autosaveTimer, SIGNAL(timeout()), this, SLOT(onAutoSaveTimer()));
	m_autosaveTimer.start( 60 * 1000 );


#ifdef H2CORE_HAVE_LASH

	if ( Preferences::get_instance()->useLash() ){
		LashClient* lashClient = LashClient::get_instance();
		if (lashClient->isConnected())
		{
			// send alsa client id now since it can only be sent
			// after the audio engine has been started.
			Preferences *pref = Preferences::get_instance();
			if ( pref->m_sMidiDriver == "ALSA" ) {
				//			infoLog("[LASH] Sending alsa seq id to LASH server");
				lashClient->sendAlsaClientId();
			}
			// start timer for polling lash events
			lashPollTimer = new QTimer(this);
			connect( lashPollTimer, SIGNAL( timeout() ), this, SLOT( onLashPollTimer() ) );
			lashPollTimer->start(500);
		}
	}
#endif


	//playlist display timer
	QTimer *playlistDisplayTimer = new QTimer(this);
	connect( playlistDisplayTimer, SIGNAL( timeout() ), this, SLOT( onPlaylistDisplayTimer() ) );
	playlistDisplayTimer->start(30000);	// update player control at
	// ~ playlist display timer

	//beatcouter
	Hydrogen::get_instance()->setBcOffsetAdjust();
	// director
	EventQueue::get_instance()->push_event( EVENT_METRONOME, 1 );
	EventQueue::get_instance()->push_event( EVENT_METRONOME, 3 );

	undoView = new QUndoView(h2app->m_undoStack);
	undoView->setWindowTitle(tr("Undo history"));
	undoView->setWindowIcon( QPixmap( Skin::getImagePath() + "/icon16.png" ) );

	//restore last playlist
	if( Preferences::get_instance()->isRestoreLastPlaylistEnabled() ){
		bool loadlist = h2app->getPlayListDialog()->loadListByFileName( Preferences::get_instance()->getLastPlaylistFilename() );
		if( !loadlist ){
			_ERRORLOG ( "Error loading the playlist" );
		}
	}
}


MainForm::~MainForm()
{
	// remove the autosave file
	QFile file( getAutoSaveFilename() );
	file.remove();

	//if a playlist is used, we save the last playlist-path to hydrogen.conf
	Preferences::get_instance()->setLastPlaylistFilename( Playlist::get_instance()->get_filename() );

	if ( (Hydrogen::get_instance()->getState() == STATE_PLAYING) ) {
		Hydrogen::get_instance()->sequencer_stop();
	}

	// remove the autosave file
	m_autosaveTimer.stop();
	QFile autosaveFile( "hydrogen_autosave.h2song" );
	autosaveFile.remove();


	hide();

	if (h2app != NULL) {
		delete Playlist::get_instance();
		delete h2app;
		h2app = NULL;
	}

}

///
/// Create the menubar
///
void MainForm::createMenuBar()
{
	// menubar
	QMenuBar *m_pMenubar = new QMenuBar( this );
	setMenuBar( m_pMenubar );

	// FILE menu
	QMenu *m_pFileMenu = m_pMenubar->addMenu( trUtf8( "&Project" ) );

	m_pFileMenu->addAction( trUtf8( "&New" ), this, SLOT( action_file_new() ), QKeySequence( "Ctrl+N" ) );
	m_pFileMenu->addAction( trUtf8( "Show &info" ), this, SLOT( action_file_songProperties() ), QKeySequence( "" ) );

	m_pFileMenu->addSeparator();				// -----

	m_pFileMenu->addAction( trUtf8( "&Open" ), this, SLOT( action_file_open() ), QKeySequence( "Ctrl+O" ) );
	m_pFileMenu->addAction( trUtf8( "Open &Demo" ), this, SLOT( action_file_openDemo() ), QKeySequence( "Ctrl+D" ) );

	m_pRecentFilesMenu = m_pFileMenu->addMenu( trUtf8( "Open &recent" ) );

	m_pFileMenu->addSeparator();				// -----

	m_pFileMenu->addAction( trUtf8( "&Save" ), this, SLOT( action_file_save() ), QKeySequence( "Ctrl+S" ) );
	m_pFileMenu->addAction( trUtf8( "Save &as..." ), this, SLOT( action_file_save_as() ), QKeySequence( "Ctrl+Shift+S" ) );

	m_pFileMenu->addSeparator();				// -----

	m_pFileMenu->addAction ( trUtf8 ( "Open &Pattern" ), this, SLOT ( action_file_openPattern() ), QKeySequence ( "" ) );
	m_pFileMenu->addAction( trUtf8( "Expor&t pattern as..." ), this, SLOT( action_file_export_pattern_as() ), QKeySequence( "Ctrl+P" ) );

	m_pFileMenu->addSeparator();				// -----

	m_pFileMenu->addAction( trUtf8( "Export &MIDI file" ), this, SLOT( action_file_export_midi() ), QKeySequence( "Ctrl+M" ) );
	m_pFileMenu->addAction( trUtf8( "&Export song" ), this, SLOT( action_file_export() ), QKeySequence( "Ctrl+E" ) );


#ifndef Q_OS_MACX
	m_pFileMenu->addSeparator();				// -----

	m_pFileMenu->addAction( trUtf8("&Quit"), this, SLOT( action_file_exit() ), QKeySequence( "Ctrl+Q" ) );
#endif

	updateRecentUsedSongList();
	connect( m_pRecentFilesMenu, SIGNAL( triggered(QAction*) ), this, SLOT( action_file_open_recent(QAction*) ) );
	//~ FILE menu

	// Undo menu
	QMenu *m_pUndoMenu = m_pMenubar->addMenu( trUtf8( "&Undo" ) );
	m_pUndoMenu->addAction( trUtf8( "Undo" ), this, SLOT( action_undo() ), QKeySequence( "Ctrl+Z" ) );
	m_pUndoMenu->addAction( trUtf8( "Redo" ), this, SLOT( action_redo() ), QKeySequence( "Shift+Ctrl+Z" ) );
	m_pUndoMenu->addAction( trUtf8( "Undo history" ), this, SLOT( openUndoStack() ), QKeySequence( "" ) );

	// INSTRUMENTS MENU
	QMenu *m_pInstrumentsMenu = m_pMenubar->addMenu( trUtf8( "I&nstruments" ) );
	m_pInstrumentsMenu->addAction( trUtf8( "&Add instrument" ), this, SLOT( action_instruments_addInstrument() ), QKeySequence( "" ) );
	m_pInstrumentsMenu->addAction( trUtf8( "&Clear all" ), this, SLOT( action_instruments_clearAll() ), QKeySequence( "" ) );
	m_pInstrumentsMenu->addAction( trUtf8( "&Save library" ), this, SLOT( action_instruments_saveLibrary() ), QKeySequence( "" ) );
	m_pInstrumentsMenu->addAction( trUtf8( "&Export library" ), this, SLOT( action_instruments_exportLibrary() ), QKeySequence( "" ) );
	m_pInstrumentsMenu->addAction( trUtf8( "&Import library" ), this, SLOT( action_instruments_importLibrary() ), QKeySequence( "" ) );




	// Tools menu
	QMenu *m_pToolsMenu = m_pMenubar->addMenu( trUtf8( "&Tools" ));

	m_pToolsMenu->addAction( trUtf8("Playlist &editor"), this, SLOT( action_window_showPlaylistDialog() ), QKeySequence( "" ) );
	m_pToolsMenu->addAction( trUtf8("Director"), this, SLOT( action_window_show_DirectorWidget() ), QKeySequence( "Alt+D" ) );

	m_pToolsMenu->addAction( trUtf8("&Mixer"), this, SLOT( action_window_showMixer() ), QKeySequence( "Alt+M" ) );

	m_pToolsMenu->addAction( trUtf8("&Instrument Rack"), this, SLOT( action_window_showDrumkitManagerPanel() ), QKeySequence( "Alt+I" ) );


	m_pInputModeMenu = m_pToolsMenu->addMenu( trUtf8( "Input mode" ) );
	m_pInstrumentAction = m_pInputModeMenu->addAction( trUtf8( "Instrument" ), this, SLOT( action_toggle_input_mode()), QKeySequence( "Ctrl+Alt+I" ) );
	m_pInstrumentAction->setCheckable( true );

	m_pDrumkitAction = m_pInputModeMenu->addAction( trUtf8( "Drumkit" ), this, SLOT( action_toggle_input_mode()), QKeySequence( "Ctrl+Alt+D" ) );
	m_pDrumkitAction->setCheckable( true );

	if( Preferences::get_instance()->__playselectedinstrument )
	{
		m_pInstrumentAction->setChecked( true );
		m_pDrumkitAction->setChecked (false );
	} else {
		m_pInstrumentAction->setChecked( false );
		m_pDrumkitAction->setChecked (true );
	}


	m_pToolsMenu->addAction( trUtf8("&Preferences"), this, SLOT( showPreferencesDialog() ), QKeySequence( "Alt+P" ) );

	//~ Tools menu


	Logger *pLogger = Logger::get_instance();
	if ( pLogger->bit_mask() >= 1 ) {
		// DEBUG menu
		QMenu *m_pDebugMenu = m_pMenubar->addMenu( trUtf8("De&bug") );
		m_pDebugMenu->addAction( trUtf8( "Show &audio engine info" ), this, SLOT( action_debug_showAudioEngineInfo() ) );
		if(pLogger->bit_mask() == 8) // hydrogen -V8 list object map in console
			m_pDebugMenu->addAction( trUtf8( "Print Objects" ), this, SLOT( action_debug_printObjects() ) );
		//~ DEBUG menu
	}

	// INFO menu
	QMenu *m_pInfoMenu = m_pMenubar->addMenu( trUtf8( "&Info" ) );
	m_pInfoMenu->addAction( trUtf8("&User manual"), this, SLOT( showUserManual() ), QKeySequence( "Ctrl+?" ) );
	m_pInfoMenu->addSeparator();
	m_pInfoMenu->addAction( trUtf8("&About"), this, SLOT( action_help_about() ), QKeySequence( trUtf8("", "Info|About") ) );
	m_pInfoMenu->addAction( trUtf8("Report bug"), this, SLOT( action_report_bug() ));
	//~ INFO menu
}




void MainForm::onLashPollTimer()
{
#ifdef H2CORE_HAVE_LASH
	if ( Preferences::get_instance()->useLash() ){
		LashClient* client = LashClient::get_instance();

		if (!client->isConnected())
		{
			WARNINGLOG("[LASH] Not connected to server!");
			return;
		}

		bool keep_running = true;

		lash_event_t* event;

		string songFilename;
		QString filenameSong;
		Song *song = Hydrogen::get_instance()->getSong();
		// Extra parentheses for -Wparentheses
		while ( (event = client->getNextEvent()) ) {

			switch (lash_event_get_type(event)) {

			case LASH_Save_File:

				INFOLOG("[LASH] Save file");

				songFilename.append(lash_event_get_string(event));
				songFilename.append("/hydrogen.h2song");

				filenameSong = QString::fromLocal8Bit( songFilename.c_str() );
				song->set_filename( filenameSong );
				action_file_save();

				client->sendEvent(LASH_Save_File);

				break;

			case LASH_Restore_File:

				songFilename.append(lash_event_get_string(event));
				songFilename.append("/hydrogen.h2song");

				INFOLOG( QString("[LASH] Restore file: %1")
						 .arg( songFilename.c_str() ) );

				filenameSong = QString::fromLocal8Bit( songFilename.c_str() );

				openSongFile( filenameSong );

				client->sendEvent(LASH_Restore_File);

				break;

			case LASH_Quit:

				//				infoLog("[LASH] Quit!");
				keep_running = false;

				break;

			default:
				;
				//				infoLog("[LASH] Got unknown event!");

			}

			lash_event_destroy(event);

		}

		if (!keep_running)
		{
			lashPollTimer->stop();
			action_file_exit();
		}
	}
#endif
}


/// return true if the app needs to be closed.
bool MainForm::action_file_exit()
{
	bool proceed = handleUnsavedChanges();
	if(!proceed) {
		return false;
	}
	closeAll();
	return true;
}



void MainForm::action_file_new()
{
	Hydrogen * pEngine = Hydrogen::get_instance();
	if ( (pEngine->getState() == STATE_PLAYING) ) {
		pEngine->sequencer_stop();
	}

	bool proceed = handleUnsavedChanges();
	if(!proceed) {
		return;
	}

	h2app->m_undoStack->clear();
	pEngine->getTimeline()->m_timelinevector.clear();
	Song * pSong = Song::get_empty_song();
	pSong->set_filename( "" );
	h2app->setSong(pSong);
	pEngine->setSelectedPatternNumber( 0 );
	h2app->getInstrumentRack()->getSoundLibraryPanel()->update_background_color();
	h2app->getSongEditorPanel()->updatePositionRuler();
	pEngine->getTimeline()->m_timelinetagvector.clear();

	// update director tags
	EventQueue::get_instance()->push_event( EVENT_METRONOME, 2 );
	// update director songname
	EventQueue::get_instance()->push_event( EVENT_METRONOME, 3 );
}



void MainForm::action_file_save_as()
{
	Hydrogen* pEngine = Hydrogen::get_instance();

	if ( pEngine->getState() == STATE_PLAYING ) {
		  pEngine->sequencer_stop();
	}

	//std::auto_ptr<QFileDialog> fd( new QFileDialog );
	QFileDialog fd(this);
	fd.setFileMode( QFileDialog::AnyFile );
	fd.setFilter( trUtf8("Hydrogen Song (*.h2song)") );
	fd.setAcceptMode( QFileDialog::AcceptSave );
	fd.setWindowTitle( trUtf8( "Save song" ) );
	fd.setSidebarUrls( fd.sidebarUrls() << QUrl::fromLocalFile( Filesystem::songs_dir() ) );

	Song *song = pEngine->getSong();
	QString defaultFilename;
	QString lastFilename = song->get_filename();

	if ( lastFilename.isEmpty() ) {
		defaultFilename = pEngine->getSong()->__name;
		defaultFilename += ".h2song";
	}
	else {
		defaultFilename = lastFilename;
	}

	fd.selectFile( defaultFilename );

	QString filename;
	if (fd.exec() == QDialog::Accepted) {
		filename = fd.selectedFiles().first();
	}

	if ( !filename.isEmpty() ) {
		QString sNewFilename = filename;
		if ( sNewFilename.endsWith(".h2song") == false ) {
			filename += ".h2song";
		}

		song->set_filename(filename);
		action_file_save();
	}
	h2app->setScrollStatusBarMessage( trUtf8("Song saved as.") + QString(" Into: ") + defaultFilename, 2000 );
	h2app->updateWindowTitle();
}



void MainForm::action_file_save()
{
	Song *pSong = Hydrogen::get_instance()->getSong();
	QString filename = pSong->get_filename();

	if ( filename.isEmpty() ) {
		// just in case!
		return action_file_save_as();
	}

	bool saved = false;
	saved = pSong->save( filename );


	if(! saved) {
		QMessageBox::warning( this, "Hydrogen", trUtf8("Could not save song.") );
	} else {
		Preferences::get_instance()->setLastSongFilename( pSong->get_filename() );

		// add the new loaded song in the "last used song" vector
		Preferences *pPref = Preferences::get_instance();
		vector<QString> recentFiles = pPref->getRecentFiles();
		recentFiles.insert( recentFiles.begin(), filename );
		pPref->setRecentFiles( recentFiles );

		updateRecentUsedSongList();

		h2app->setScrollStatusBarMessage( trUtf8("Song saved.") + QString(" Into: ") + filename, 2000 );
		EventQueue::get_instance()->push_event( EVENT_METRONOME, 3 );
	}
}


void MainForm::action_toggle_input_mode()
{
	if(  !Preferences::get_instance()->__playselectedinstrument )
	{
		Preferences::get_instance()->__playselectedinstrument = true;
		m_pInstrumentAction->setChecked( true );
		m_pDrumkitAction->setChecked (false );
	} else {
		Preferences::get_instance()->__playselectedinstrument = false;
		m_pInstrumentAction->setChecked( false );
		m_pDrumkitAction->setChecked (true );
	}
}

void MainForm::action_help_about() {
	AboutDialog *dialog = new AboutDialog( NULL );
	dialog->exec();
}

void MainForm::action_report_bug()
{
	QDesktopServices::openUrl(QString("https://github.com/hydrogen-music/hydrogen/issues"));
}

void MainForm::showUserManual()
{
	h2app->getHelpBrowser()->hide();
	h2app->getHelpBrowser()->show();
}

void MainForm::action_file_export_pattern_as()
{
	if ( ( Hydrogen::get_instance()->getState() == STATE_PLAYING ) )
	{
		Hydrogen::get_instance()->sequencer_stop();
	}

	Hydrogen *engine = Hydrogen::get_instance();
	int selectedpattern = engine->getSelectedPatternNumber();
	Song *song = engine->getSong();
	Pattern *pat = song->get_pattern_list()->get ( selectedpattern );

	Instrument *instr = song->get_instrument_list()->get ( 0 );
	assert ( instr );

	QDir dir  = Preferences::get_instance()->__lastspatternDirectory;


	QFileDialog fd(this);
	fd.setFileMode ( QFileDialog::AnyFile );
	fd.setFilter ( trUtf8 ( "Hydrogen Pattern (*.h2pattern)" ) );
	fd.setAcceptMode ( QFileDialog::AcceptSave );
	fd.setWindowTitle ( trUtf8 ( "Save Pattern as ..." ) );
	fd.setDirectory ( dir );
	fd.setSidebarUrls( fd.sidebarUrls() << QUrl::fromLocalFile( Filesystem::patterns_dir() ) );



	QString defaultPatternname = QString ( pat->get_name() );

	fd.selectFile ( defaultPatternname );

	LocalFileMng fileMng;
	QString filename;
	if ( fd.exec() == QDialog::Accepted )
	{
		filename = fd.selectedFiles().first();
		QString tmpfilename = filename;
		QString toremove = tmpfilename.section( '/', -1 );
		QString newdatapath =  tmpfilename.replace( toremove, "" );
		Preferences::get_instance()->__lastspatternDirectory = newdatapath;
	}

	if ( !filename.isEmpty() )
	{
		QString sNewFilename = filename;
		if(sNewFilename.endsWith( ".h2pattern" ) ){
			sNewFilename += "";
		}
		else{
			sNewFilename += ".h2pattern";
		}
		QString patternname = sNewFilename;
		QString realpatternname = filename;
		QString realname = realpatternname.mid( realpatternname.lastIndexOf( "/" ) + 1 );
		if ( realname.endsWith( ".h2pattern" ) )
			realname.replace( ".h2pattern", "" );
		pat->set_name(realname);
		HydrogenApp::get_instance()->getSongEditorPanel()->updateAll();
		int err = fileMng.savePattern ( song, engine->getCurrentDrumkitname(), selectedpattern, patternname, realname, 2 );
		if ( err != 0 )
		{
			QMessageBox::warning( this, "Hydrogen", trUtf8("Could not export pattern.") );
			_ERRORLOG ( "Error saving the pattern" );
		}
	}
	h2app->setStatusBarMessage ( trUtf8 ( "Pattern saved." ), 10000 );

	//update SoundlibraryPanel
	HydrogenApp::get_instance()->getInstrumentRack()->getSoundLibraryPanel()->test_expandedItems();
	HydrogenApp::get_instance()->getInstrumentRack()->getSoundLibraryPanel()->updateDrumkitList();
}



void MainForm::action_file_open() {
	if ( ((Hydrogen::get_instance())->getState() == STATE_PLAYING) ) {
		Hydrogen::get_instance()->sequencer_stop();
	}

	bool proceed = handleUnsavedChanges();
	if(!proceed) {
		return;
	}

	static QString lastUsedDir = Preferences::get_instance()->getDataDirectory() + "/songs";

	//std::auto_ptr<QFileDialog> fd( new QFileDialog );
	QFileDialog fd(this);
	fd.setFileMode(QFileDialog::ExistingFile);
	fd.setFilter( trUtf8("Hydrogen Song (*.h2song)") );
	fd.setDirectory( lastUsedDir );

	fd.setWindowTitle( trUtf8( "Open song" ) );

	QString filename;
	if (fd.exec() == QDialog::Accepted) {
		filename = fd.selectedFiles().first();
		lastUsedDir = fd.directory().absolutePath();
	}


	if ( !filename.isEmpty() ) {
		openSongFile( filename );
	}

	HydrogenApp::get_instance()->getInstrumentRack()->getSoundLibraryPanel()->update_background_color();
}


void MainForm::action_file_openPattern()
{

	Hydrogen *pEngine = Hydrogen::get_instance();
	Song *pSong = pEngine->getSong();
	PatternList *pPatternList = pSong->get_pattern_list();

	Instrument *instr = pSong->get_instrument_list()->get ( 0 );
	assert ( instr );

	QDir dirPattern( Preferences::get_instance()->getDataDirectory() + "/patterns" );
	QFileDialog fd(this);
	fd.setFileMode ( QFileDialog::ExistingFile );
	fd.setFilter ( trUtf8 ( "Hydrogen Pattern (*.h2pattern)" ) );
	fd.setDirectory ( dirPattern );

	fd.setWindowTitle ( trUtf8 ( "Open Pattern" ) );


	QString filename;
	if ( fd.exec() == QDialog::Accepted )
	{
		filename = fd.selectedFiles().first();
	}
	QString patternname = filename;

	LocalFileMng fileMng;
	Pattern* err = fileMng.loadPattern ( patternname );
	if ( err == 0 )
	{
		_ERRORLOG( "Error loading the pattern" );
		_ERRORLOG( patternname );
	}
	else
	{
		H2Core::Pattern *pNewPattern = err;
		pPatternList->add ( pNewPattern );
		pSong->set_is_modified( true );
		EventQueue::get_instance()->push_event( EVENT_SONG_MODIFIED, -1 );
	}

	HydrogenApp::get_instance()->getSongEditorPanel()->updateAll();
}

/// \todo parametrizzare il metodo action_file_open ed eliminare il seguente...
void MainForm::action_file_openDemo()
{
	if ( (Hydrogen::get_instance()->getState() == STATE_PLAYING) ) {
		Hydrogen::get_instance()->sequencer_stop();
	}

	bool proceed = handleUnsavedChanges();
	if(!proceed) {
		return;
	}

	h2app->m_undoStack->clear();
	QFileDialog fd(this);
	fd.setFileMode(QFileDialog::ExistingFile);
	fd.setFilter( trUtf8("Hydrogen Song (*.h2song)") );

	fd.setWindowTitle( trUtf8( "Open song" ) );
	fd.setWindowIcon( QPixmap( Skin::getImagePath() + "/icon16.png" ) );

	fd.setDirectory( QString( Preferences::get_instance()->getDemoPath() ) );


	QString filename;
	if (fd.exec() == QDialog::Accepted) {
		filename = fd.selectedFiles().first();
	}


	if ( !filename.isEmpty() ) {
		openSongFile( filename );
		Hydrogen::get_instance()->getSong()->set_filename( "" );
	}
}



void MainForm::showPreferencesDialog()
{
	h2app->showPreferencesDialog();
}



void MainForm::action_window_showPlaylistDialog()
{
	h2app->showPlaylistDialog();
}

void MainForm::action_window_show_DirectorWidget()
{

	h2app->showDirector();
}

void MainForm::action_window_showMixer()
{
	bool isVisible = HydrogenApp::get_instance()->getMixer()->isVisible();
	h2app->showMixer( !isVisible );
}



void MainForm::action_debug_showAudioEngineInfo()
{
	h2app->showAudioEngineInfoForm();
}



///
/// Shows the song editor
///
void MainForm::action_window_showSongEditor()
{
	bool isVisible = h2app->getSongEditorPanel()->isVisible();
	h2app->getSongEditorPanel()->setHidden( isVisible );
}



void MainForm::action_instruments_addInstrument()
{
	SE_mainMenuAddInstrumentAction *action = new SE_mainMenuAddInstrumentAction();
	HydrogenApp::get_instance()->m_undoStack->push( action );
}



void MainForm::action_instruments_clearAll()
{
	switch(
		   QMessageBox::information( this,
									 "Hydrogen",
									 trUtf8("Clear all instruments?"),
									 trUtf8("Ok"),
									 trUtf8("Cancel"),
									 0,      // Enter == button 0
									 1 )) { // Escape == button 2
	case 0:
		// ok btn pressed
		break;
	case 1:
		// cancel btn pressed
		return;
	}

	// Remove all layers

	Song *pSong = Hydrogen::get_instance()->getSong();
	InstrumentList* pList = pSong->get_instrument_list();
	for (uint i = pList->size(); i > 0; i--) {
		functionDeleteInstrument(i - 1);
	}

	EventQueue::get_instance()->push_event( EVENT_SELECTED_INSTRUMENT_CHANGED, -1 );
}

void MainForm::functionDeleteInstrument(int instrument)
{
	Hydrogen * H = Hydrogen::get_instance();
	Instrument *pSelectedInstrument = H->getSong()->get_instrument_list()->get( instrument );

	std::list< Note* > noteList;
	Song* song = H->getSong();
	PatternList *patList = song->get_pattern_list();

	QString instrumentName =  pSelectedInstrument->get_name();
	QString drumkitName = H->getCurrentDrumkitname();

	for ( int i = 0; i < patList->size(); i++ ) {
		H2Core::Pattern *pPattern = song->get_pattern_list()->get(i);
		const Pattern::notes_t* notes = pPattern->get_notes();
		FOREACH_NOTE_CST_IT_BEGIN_END(notes,it) {
			Note *pNote = it->second;
			assert( pNote );
			if ( pNote->get_instrument() == pSelectedInstrument ) {
				pNote->set_pattern_idx( i );
				noteList.push_back( pNote );
			}
		}
	}
	SE_deleteInstrumentAction *action = new SE_deleteInstrumentAction( noteList, drumkitName, instrumentName, instrument );
	HydrogenApp::get_instance()->m_undoStack->push( action );
}


void MainForm::action_instruments_exportLibrary()
{
	SoundLibraryExportDialog exportDialog( this, QString() );
	exportDialog.exec();
}




void MainForm::action_instruments_importLibrary()
{
	SoundLibraryImportDialog dialog( this );
	dialog.exec();
}



void MainForm::action_instruments_saveLibrary()
{
	SoundLibrarySaveDialog dialog( this );
	dialog.exec();
	HydrogenApp::get_instance()->getInstrumentRack()->getSoundLibraryPanel()->test_expandedItems();
	HydrogenApp::get_instance()->getInstrumentRack()->getSoundLibraryPanel()->updateDrumkitList();
}







///
/// Window close event
///
void MainForm::closeEvent( QCloseEvent* ev )
{
	if ( action_file_exit() == false ) {
		// don't close!!!
		ev->ignore();
		return;
	}

	ev->accept();
}



void MainForm::action_file_export() {
	if ( (Hydrogen::get_instance()->getState() == STATE_PLAYING) ) {
		Hydrogen::get_instance()->sequencer_stop();
	}

	ExportSongDialog *dialog = new ExportSongDialog(this);
	dialog->exec();
	delete dialog;
}



void MainForm::action_window_showDrumkitManagerPanel()
{
	InstrumentRack *pPanel = HydrogenApp::get_instance()->getInstrumentRack();
	pPanel->setHidden( pPanel->isVisible() );
}




void MainForm::closeAll() {
	// save window properties in the preferences files
	Preferences *pref = Preferences::get_instance();

	// mainform
	WindowProperties mainFormProp;
	mainFormProp.x = x();
	mainFormProp.y = y();
	mainFormProp.height = height();
	mainFormProp.width = width();
	pref->setMainFormProperties( mainFormProp );

	// Save mixer properties
	WindowProperties mixerProp;
	mixerProp.x = h2app->getMixer()->x();
	mixerProp.y = h2app->getMixer()->y();
	mixerProp.width = h2app->getMixer()->width();
	mixerProp.height = h2app->getMixer()->height();
	mixerProp.visible = h2app->getMixer()->isVisible();
	pref->setMixerProperties( mixerProp );

	// save pattern editor properties
	WindowProperties patternEditorProp;
	patternEditorProp.x = h2app->getPatternEditorPanel()->x();
	patternEditorProp.y = h2app->getPatternEditorPanel()->y();
	patternEditorProp.width = h2app->getPatternEditorPanel()->width();
	patternEditorProp.height = h2app->getPatternEditorPanel()->height();
	patternEditorProp.visible = h2app->getPatternEditorPanel()->isVisible();
	pref->setPatternEditorProperties( patternEditorProp );

	// save song editor properties
	WindowProperties songEditorProp;
	songEditorProp.x = h2app->getSongEditorPanel()->x();
	songEditorProp.y = h2app->getSongEditorPanel()->y();
	songEditorProp.width = h2app->getSongEditorPanel()->width();
	songEditorProp.height = h2app->getSongEditorPanel()->height();

	QSize size = h2app->getSongEditorPanel()->frameSize();
	songEditorProp.visible = h2app->getSongEditorPanel()->isVisible();
	pref->setSongEditorProperties( songEditorProp );


	// save audio engine info properties
	WindowProperties audioEngineInfoProp;
	audioEngineInfoProp.x = h2app->getAudioEngineInfoForm()->x();
	audioEngineInfoProp.y = h2app->getAudioEngineInfoForm()->y();
	audioEngineInfoProp.visible = h2app->getAudioEngineInfoForm()->isVisible();
	pref->setAudioEngineInfoProperties( audioEngineInfoProp );


#ifdef H2CORE_HAVE_LADSPA
	// save LADSPA FX window properties
	for (uint nFX = 0; nFX < MAX_FX; nFX++) {
		WindowProperties prop;
		prop.x = h2app->getLadspaFXProperties(nFX)->x();
		prop.y = h2app->getLadspaFXProperties(nFX)->y();
		prop.visible= h2app->getLadspaFXProperties(nFX)->isVisible();
		pref->setLadspaProperties(nFX, prop);
	}
#endif

	m_pQApp->quit();
}



// keybindings..

void MainForm::onPlayStopAccelEvent()
{
	int nState = Hydrogen::get_instance()->getState();
	switch (nState) {
	case STATE_READY:
		Hydrogen::get_instance()->sequencer_play();
		break;

	case STATE_PLAYING:
		Hydrogen::get_instance()->sequencer_stop();
		break;

	default:
		ERRORLOG( "[MainForm::onPlayStopAccelEvent()] Unhandled case." );
	}
}



void MainForm::onRestartAccelEvent()
{
	Hydrogen* pEngine = Hydrogen::get_instance();
	pEngine->setPatternPos( 0 );
}



void MainForm::onBPMPlusAccelEvent()
{
	Hydrogen* pEngine = Hydrogen::get_instance();
	AudioEngine::get_instance()->lock( RIGHT_HERE );

	Song* pSong = pEngine->getSong();
	if (pSong->__bpm  < 300) {
		pEngine->setBPM( pSong->__bpm + 0.1 );
	}
	AudioEngine::get_instance()->unlock();
}



void MainForm::onBPMMinusAccelEvent()
{
	Hydrogen* pEngine = Hydrogen::get_instance();
	AudioEngine::get_instance()->lock( RIGHT_HERE );

	Song* pSong = pEngine->getSong();
	if (pSong->__bpm > 40 ) {
		pEngine->setBPM( pSong->__bpm - 0.1 );
	}
	AudioEngine::get_instance()->unlock();
}



void MainForm::onSaveAsAccelEvent()
{
	action_file_save_as();
}



void MainForm::onSaveAccelEvent()
{
	action_file_save();
}



void MainForm::onOpenAccelEvent()
{
	action_file_open();
}



void MainForm::updateRecentUsedSongList()
{
	m_pRecentFilesMenu->clear();

	Preferences *pPref = Preferences::get_instance();
	vector<QString> recentUsedSongs = pPref->getRecentFiles();

	QString sFilename;

	for ( uint i = 0; i < recentUsedSongs.size(); ++i ) {
		sFilename = recentUsedSongs[ i ];

		if ( !sFilename.isEmpty() ) {
			QAction *pAction = new QAction( this  );
			pAction->setText( sFilename );
			m_pRecentFilesMenu->addAction( pAction );
		}
	}
}



void MainForm::action_file_open_recent(QAction *pAction)
{
	//	INFOLOG( pAction->text() );
	openSongFile( pAction->text() );
}



void MainForm::openSongFile( const QString& sFilename )
{
	Hydrogen *engine = Hydrogen::get_instance();
	if ( engine->getState() == STATE_PLAYING ) {
		engine->sequencer_stop();
	}

	engine->getTimeline()->m_timelinetagvector.clear();

	h2app->closeFXProperties();

	Song *pSong = Song::load( sFilename );
	if ( pSong == NULL ) {
		QMessageBox::information( this, "Hydrogen", trUtf8("Error loading song.") );
		return;
	}

	h2app->m_undoStack->clear();

	// add the new loaded song in the "last used song" vector
	Preferences *pPref = Preferences::get_instance();
	vector<QString> recentFiles = pPref->getRecentFiles();
	recentFiles.insert( recentFiles.begin(), sFilename );
	pPref->setRecentFiles( recentFiles );

	h2app->setSong( pSong );

	updateRecentUsedSongList();
	engine->setSelectedPatternNumber( 0 );
	HydrogenApp::get_instance()->getSongEditorPanel()->updatePositionRuler();
	EventQueue::get_instance()->push_event( EVENT_METRONOME, 3 );
}



void MainForm::initKeyInstMap()
{

	QString loc = QLocale::system().name();
	int instr = 0;

	///POSIX Locale
	//locale for keyboardlayout QWERTZ
	// de_DE, de_AT, de_LU, de_CH, de

	//locale for keyboardlayout AZERTY
	// fr_BE, fr_CA, fr_FR, fr_LU, fr_CH

	//locale for keyboardlayout QWERTY
	// en_GB, en_US, en_ZA, usw.

	if ( loc.contains( "de" ) || loc.contains( "DE" )){ ///QWERTZ
		keycodeInstrumentMap[Qt::Key_Y] = instr++;
		keycodeInstrumentMap[Qt::Key_S] = instr++;
		keycodeInstrumentMap[Qt::Key_X] = instr++;
		keycodeInstrumentMap[Qt::Key_D] = instr++;
		keycodeInstrumentMap[Qt::Key_C] = instr++;
		keycodeInstrumentMap[Qt::Key_V] = instr++;
		keycodeInstrumentMap[Qt::Key_G] = instr++;
		keycodeInstrumentMap[Qt::Key_B] = instr++;
		keycodeInstrumentMap[Qt::Key_H] = instr++;
		keycodeInstrumentMap[Qt::Key_N] = instr++;
		keycodeInstrumentMap[Qt::Key_J] = instr++;
		keycodeInstrumentMap[Qt::Key_M] = instr++;

		keycodeInstrumentMap[Qt::Key_Q] = instr++;
		keycodeInstrumentMap[Qt::Key_2] = instr++;
		keycodeInstrumentMap[Qt::Key_W] = instr++;
		keycodeInstrumentMap[Qt::Key_3] = instr++;
		keycodeInstrumentMap[Qt::Key_E] = instr++;
		keycodeInstrumentMap[Qt::Key_R] = instr++;
		keycodeInstrumentMap[Qt::Key_5] = instr++;
		keycodeInstrumentMap[Qt::Key_T] = instr++;
		keycodeInstrumentMap[Qt::Key_6] = instr++;
		keycodeInstrumentMap[Qt::Key_Z] = instr++;
		keycodeInstrumentMap[Qt::Key_7] = instr++;
		keycodeInstrumentMap[Qt::Key_U] = instr++;
	}
	else if ( loc.contains( "fr" ) || loc.contains( "FR" )){ ///AZERTY
		keycodeInstrumentMap[Qt::Key_W] = instr++;
		keycodeInstrumentMap[Qt::Key_S] = instr++;
		keycodeInstrumentMap[Qt::Key_X] = instr++;
		keycodeInstrumentMap[Qt::Key_D] = instr++;
		keycodeInstrumentMap[Qt::Key_C] = instr++;
		keycodeInstrumentMap[Qt::Key_V] = instr++;
		keycodeInstrumentMap[Qt::Key_G] = instr++;
		keycodeInstrumentMap[Qt::Key_B] = instr++;
		keycodeInstrumentMap[Qt::Key_H] = instr++;
		keycodeInstrumentMap[Qt::Key_N] = instr++;
		keycodeInstrumentMap[Qt::Key_J] = instr++;
		keycodeInstrumentMap[Qt::Key_Question] = instr++;

		keycodeInstrumentMap[Qt::Key_A] = instr++;
		keycodeInstrumentMap[Qt::Key_2] = instr++;
		keycodeInstrumentMap[Qt::Key_Z] = instr++;
		keycodeInstrumentMap[Qt::Key_3] = instr++;
		keycodeInstrumentMap[Qt::Key_E] = instr++;
		keycodeInstrumentMap[Qt::Key_R] = instr++;
		keycodeInstrumentMap[Qt::Key_5] = instr++;
		keycodeInstrumentMap[Qt::Key_T] = instr++;
		keycodeInstrumentMap[Qt::Key_6] = instr++;
		keycodeInstrumentMap[Qt::Key_Y] = instr++;
		keycodeInstrumentMap[Qt::Key_7] = instr++;
		keycodeInstrumentMap[Qt::Key_U] = instr++;
	}else
	{ /// default QWERTY
		keycodeInstrumentMap[Qt::Key_Z] = instr++;
		keycodeInstrumentMap[Qt::Key_S] = instr++;
		keycodeInstrumentMap[Qt::Key_X] = instr++;
		keycodeInstrumentMap[Qt::Key_D] = instr++;
		keycodeInstrumentMap[Qt::Key_C] = instr++;
		keycodeInstrumentMap[Qt::Key_V] = instr++;
		keycodeInstrumentMap[Qt::Key_G] = instr++;
		keycodeInstrumentMap[Qt::Key_B] = instr++;
		keycodeInstrumentMap[Qt::Key_H] = instr++;
		keycodeInstrumentMap[Qt::Key_N] = instr++;
		keycodeInstrumentMap[Qt::Key_J] = instr++;
		keycodeInstrumentMap[Qt::Key_M] = instr++;

		keycodeInstrumentMap[Qt::Key_Q] = instr++;
		keycodeInstrumentMap[Qt::Key_2] = instr++;
		keycodeInstrumentMap[Qt::Key_W] = instr++;
		keycodeInstrumentMap[Qt::Key_3] = instr++;
		keycodeInstrumentMap[Qt::Key_E] = instr++;
		keycodeInstrumentMap[Qt::Key_R] = instr++;
		keycodeInstrumentMap[Qt::Key_5] = instr++;
		keycodeInstrumentMap[Qt::Key_T] = instr++;
		keycodeInstrumentMap[Qt::Key_6] = instr++;
		keycodeInstrumentMap[Qt::Key_Y] = instr++;
		keycodeInstrumentMap[Qt::Key_7] = instr++;
		keycodeInstrumentMap[Qt::Key_U] = instr++;
	}
}



bool MainForm::eventFilter( QObject *o, QEvent *e )
{
	UNUSED( o );

	if ( e->type() == QEvent::KeyPress) {
		// special processing for key press
		QKeyEvent *k = (QKeyEvent *)e;

		// qDebug( "Got key press for instrument '%c'", k->ascii() );
		//int songnumber = 0;
		HydrogenApp* app = HydrogenApp::get_instance();
		Hydrogen* engine = Hydrogen::get_instance();
		switch (k->key()) {
		case Qt::Key_Space:
			onPlayStopAccelEvent();
			return TRUE; // eat event


		case Qt::Key_Comma:
			engine->handleBeatCounter();
			return TRUE; // eat even
			break;

		case Qt::Key_Backspace:
			onRestartAccelEvent();
			return TRUE; // eat event
			break;

		case Qt::Key_Plus:
			onBPMPlusAccelEvent();
			return TRUE; // eat event
			break;

		case Qt::Key_Minus:
			onBPMMinusAccelEvent();
			return TRUE; // eat event
			break;

		case Qt::Key_Backslash:
			engine->onTapTempoAccelEvent();
			return TRUE; // eat event
			break;

		case  Qt::Key_S | Qt::CTRL:
			onSaveAccelEvent();
			return TRUE;
			break;

		case  Qt::Key_F5 :
			if( engine->m_PlayList.size() == 0)
				break;
			return handleSelectNextPrevSongOnPlaylist( -1 );
			break;

		case  Qt::Key_F6 :
			if( Hydrogen::get_instance()->m_PlayList.size() == 0)
				break;
			return handleSelectNextPrevSongOnPlaylist( 1 );
			break;

		case  Qt::Key_F12 : //panic button stop all playing notes
			engine->__panic();
			//				QMessageBox::information( this, "Hydrogen", trUtf8( "Panic" ) );
			return TRUE;
			break;

		case  Qt::Key_F9 : // Qt::Key_Left do not work. Some ideas ?
			engine->setPatternPos( Hydrogen::get_instance()->getPatternPos() - 1 );
			return TRUE;
			break;

		case  Qt::Key_F10 : // Qt::Key_Right do not work. Some ideas ?
			engine->setPatternPos( Hydrogen::get_instance()->getPatternPos() + 1 );
			return TRUE;
			break;

		case Qt::Key_L :
			engine->togglePlaysSelected();
			QString msg = Preferences::get_instance()->patternModePlaysSelected() ? "Single pattern mode" : "Stacked pattern mode";
			app->setStatusBarMessage( msg, 5000 );
			app->getSongEditorPanel()->setModeActionBtn( Preferences::get_instance()->patternModePlaysSelected() );
			app->getSongEditorPanel()->updateAll();
			return TRUE;
			break;
		}

		// virtual keyboard handling
		map<int,int>::iterator found = keycodeInstrumentMap.find ( k->key() );
		if (found != keycodeInstrumentMap.end()) {
			//			INFOLOG( "[eventFilter] virtual keyboard event" );
			// insert note at the current column in time
			// if event recording enabled
			int row = (*found).second;

			float velocity = 0.8;
			float pan_L = 0.5f;
			float pan_R = 0.5f;

			engine->addRealtimeNote (row, velocity, pan_L, pan_R, 0, false, false , row + 36);

			return TRUE; // eat event
		}
		else {
			return FALSE; // let it go
		}
	}
	else {
		return FALSE; // standard event processing
	}
}





/// print the object map
void MainForm::action_debug_printObjects()
{
	INFOLOG( "[action_debug_printObjects]" );
	Object::write_objects_map_to_cerr();
}






void MainForm::action_file_export_midi()
{
	if ( ((Hydrogen::get_instance())->getState() == STATE_PLAYING) ) {
		Hydrogen::get_instance()->sequencer_stop();
	}

	QFileDialog fd(this);
	fd.setFileMode(QFileDialog::AnyFile);
	fd.setFilter( trUtf8("Midi file (*.mid)") );
	fd.setDirectory( QDir::homePath() );
	fd.setWindowTitle( trUtf8( "Export MIDI file" ) );
	fd.setAcceptMode( QFileDialog::AcceptSave );
	fd.setWindowIcon( QPixmap( Skin::getImagePath() + "/icon16.png" ) );

	QString sFilename;
	if ( fd.exec() == QDialog::Accepted ) {
		sFilename = fd.selectedFiles().first();
	}

	if ( !sFilename.isEmpty() ) {
		if ( sFilename.endsWith(".mid") == false ) {
			sFilename += ".mid";
		}

		Song *pSong = Hydrogen::get_instance()->getSong();

		// create the Standard Midi File object
		SMFWriter *pSmfWriter = new SMFWriter();
		pSmfWriter->save( sFilename, pSong );

		delete pSmfWriter;
	}
}



void MainForm::errorEvent( int nErrorCode )
{
	//ERRORLOG( "[errorEvent]" );

	QString msg;
	switch (nErrorCode) {
	case Hydrogen::UNKNOWN_DRIVER:
		msg = trUtf8( "Unknown audio driver" );
		break;

	case Hydrogen::ERROR_STARTING_DRIVER:
		msg = trUtf8( "Error starting audio driver" );
		break;

	case Hydrogen::JACK_SERVER_SHUTDOWN:
		msg = trUtf8( "Jack driver: server shutdown" );
		break;

	case Hydrogen::JACK_CANNOT_ACTIVATE_CLIENT:
		msg = trUtf8( "Jack driver: cannot activate client" );
		break;

	case Hydrogen::JACK_CANNOT_CONNECT_OUTPUT_PORT:
		msg = trUtf8( "Jack driver: cannot connect output port" );
		break;

	case Hydrogen::JACK_ERROR_IN_PORT_REGISTER:
		msg = trUtf8( "Jack driver: error in port register" );
		break;

	default:
		msg = QString( trUtf8( "Unknown error %1" ) ).arg( nErrorCode );
	}
	QMessageBox::information( this, "Hydrogen", msg );
}

void MainForm::playlistLoadSongEvent (int nIndex)
{
	Playlist* pPlaylist = Playlist::get_instance();

	if ( ! pPlaylist->loadSong ( nIndex ) )
		return;

	Song* pSong = Hydrogen::get_instance()->getSong();

	h2app->getSongEditorPanel()->updateAll();
	h2app->getPatternEditorPanel()->updateSLnameLabel();

	QString songName( pSong->__name );
	if( songName == "Untitled Song" && !pSong->get_filename().isEmpty() ){
		songName = pSong->get_filename();
		songName = songName.section( '/', -1 );
	}
	setWindowTitle( songName  );

	h2app->getMainForm()->updateRecentUsedSongList();
	h2app->closeFXProperties();
	h2app->m_undoStack->clear();

	EventQueue::get_instance()->push_event( EVENT_METRONOME, 3 );
	HydrogenApp::get_instance()->setScrollStatusBarMessage( trUtf8( "Playlist: Set song No. %1" ).arg( nIndex +1 ), 5000 );
}

void MainForm::jacksessionEvent( int nEvent )
{
	switch (nEvent){
	case 0:
		action_file_save();
		break;
	case 1:
		action_file_exit();
		break;
	}

}

void MainForm::action_file_songProperties()
{
	SongPropertiesDialog *pDialog = new SongPropertiesDialog( this );
	if ( pDialog->exec() == QDialog::Accepted ) {
		Hydrogen::get_instance()->getSong()->set_is_modified( true );
	}
	delete pDialog;
}


void MainForm::action_window_showPatternEditor()
{
	bool isVisible = HydrogenApp::get_instance()->getPatternEditorPanel()->isVisible();
	HydrogenApp::get_instance()->getPatternEditorPanel()->setHidden( isVisible );
}


void MainForm::showDevelWarning()
{

	//set this to 'false' for the case that you want to make a release..
	if ( true ) {
		Preferences *pref = Preferences::get_instance();
		bool isDevelWarningEnabled = pref->getShowDevelWarning();
		if(isDevelWarningEnabled) {

			QString msg = trUtf8( "You're using a development version of Hydrogen, please help us reporting bugs or suggestions in the hydrogen-devel mailing list.<br><br>Thank you!" );
			QMessageBox develMessageBox( this );
			develMessageBox.setText( msg );
			develMessageBox.addButton( QMessageBox::Ok );
			develMessageBox.addButton( trUtf8( "Don't show this message anymore" ) , QMessageBox::AcceptRole );

			if( develMessageBox.exec() == 0 ){
				//don't show warning again
				pref->setShowDevelWarning( false );
			}
		}


	}
}



QString MainForm::getAutoSaveFilename()
{
	Song *pSong = Hydrogen::get_instance()->getSong();
	assert( pSong );
	QString sOldFilename = pSong->get_filename();
	QString newName = "autosave.h2song";

	if ( !sOldFilename.isEmpty() ) {
		newName = sOldFilename.left( sOldFilename.length() - 7 ) + ".autosave.h2song";
	}

	return newName;
}



void MainForm::onAutoSaveTimer()
{
	//INFOLOG( "[onAutoSaveTimer]" );
	Song *pSong = Hydrogen::get_instance()->getSong();
	assert( pSong );
	QString sOldFilename = pSong->get_filename();

	pSong->save( getAutoSaveFilename() );

	pSong->set_filename(sOldFilename);
}


void MainForm::onPlaylistDisplayTimer()
{
	if( Hydrogen::get_instance()->m_PlayList.size() == 0)
		return;
	int songnumber = Playlist::get_instance()->getActiveSongNumber();
	QString songname;
	if ( songnumber == -1 )
		return;

	if ( Hydrogen::get_instance()->getSong()->__name == "Untitled Song" ){
		songname = Hydrogen::get_instance()->getSong()->get_filename();
	}else
	{
		songname = Hydrogen::get_instance()->getSong()->__name;
	}
	QString message = (trUtf8("Playlist: Song No. %1").arg( songnumber + 1)) + QString("  ---  Songname: ") + songname + QString("  ---  Author: ") + Hydrogen::get_instance()->getSong()->__author;
	HydrogenApp::get_instance()->setScrollStatusBarMessage( message, 2000 );
}

// Returns true if unsaved changes are successfully handled (saved, discarded, etc.)
// Returns false if not (i.e. Cancel)
bool MainForm::handleUnsavedChanges()
{
	bool done = false;
	bool rv = true;
	while ( !done && Hydrogen::get_instance()->getSong()->get_is_modified() ) {
		switch(
			   QMessageBox::information( this, "Hydrogen",
										 trUtf8("\nThe document contains unsaved changes.\n"
												"Do you want to save the changes?\n"),
										 trUtf8("&Save"), trUtf8("&Discard"), trUtf8("&Cancel"),
										 0,      // Enter == button 0
										 2 ) ) { // Escape == button 2
		case 0: // Save clicked or Alt+S pressed or Enter pressed.
			// If the save fails, the __is_modified flag will still be true
			if ( ! Hydrogen::get_instance()->getSong()->get_filename().isEmpty() ) {
				action_file_save();
			} else {
				// never been saved
				action_file_save_as();
			}
			// save
			break;
		case 1: // Discard clicked or Alt+D pressed
			// don't save but exit
			done = true;
			break;
		case 2: // Cancel clicked or Alt+C pressed or Escape pressed
			// don't exit
			done = true;
			rv = false;
			break;
		}
	}

	if(rv != false)
	{
		while ( !done && Playlist::get_instance()->getIsModified() ) {
			switch(
					QMessageBox::information( this, "Hydrogen",
											 trUtf8("\nThe current playlist contains unsaved changes.\n"
													"Do you want to discard the changes?\n"),
											trUtf8("&Discard"), trUtf8("&Cancel"),
											 0,      // Enter == button 0
											 2 ) ) { // Escape == button 1
			case 0: // Discard clicked or Alt+D pressed
				// don't save but exit
				done = true;
				break;
			case 1: // Cancel clicked or Alt+C pressed or Escape pressed
				// don't exit
				done = true;
				rv = false;
				break;
			}
		}
	}


	return rv;
}


void MainForm::usr1SignalHandler(int)
{
	char a = 1;
	size_t ret = ::write(sigusr1Fd[0], &a, sizeof(a));
}

void MainForm::handleSigUsr1()
{
	snUsr1->setEnabled(false);
	char tmp;
	size_t ret = ::read(sigusr1Fd[1], &tmp, sizeof(tmp));

	action_file_save();
	snUsr1->setEnabled(true);
}

void MainForm::openUndoStack()
{
	undoView->show();
	undoView->setAttribute(Qt::WA_QuitOnClose, false);
}

void MainForm::action_undo(){
	h2app->m_undoStack->undo();
}

void MainForm::action_redo(){
	h2app->m_undoStack->redo();
}

void MainForm::undoRedoActionEvent( int nEvent ){
	if(nEvent == 0)
		h2app->m_undoStack->undo();
	else if(nEvent == 1)
		h2app->m_undoStack->redo();
}

bool MainForm::handleSelectNextPrevSongOnPlaylist( int step )
{
	int playlistSize= Hydrogen::get_instance()->m_PlayList.size();

	HydrogenApp* app = HydrogenApp::get_instance();
	int songnumber = Playlist::get_instance()->getActiveSongNumber();
	if(songnumber+step >= 0 && songnumber+step <= playlistSize-1){
		Playlist::get_instance()->setNextSongByNumber( songnumber + step );
	}
	else
		return FALSE;

	return TRUE;
}



