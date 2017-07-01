#include <QtWidgets>

#include "mainwindow.h"
#include "aboutdialog.h"
#include "src/gbl/gbl_version.h"
#include "src/gbl/gbl_historymodel.h"
#include "src/gbl/gbl_filemodel.h"
#include "src/gbl/gbl_refsmodel.h"
#include "src/ui/historyview.h"
#include "src/ui/fileview.h"
#include "referencesview.h"
#include "diffview.h"
#include "clonedialog.h"
#include "src/gbl/gbl_storage.h"
#include "commitdetail.h"
#include "prefsdialog.h"
#include "urlpixmap.h"
#include "stageddockview.h"
#include "unstageddockview.h"
#include "toolbarcombo.h"
#include "badgetoolbutton.h"

#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QDir>
#include <QFileInfo>
#include <QSplitter>

#define UPDATE_STATUS_INTERVAL 30000

MainWindow* MainWindow::m_pSingleInst = NULL;


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_qpRepo = NULL;
    m_pHistModel = NULL;
    m_pHistView = NULL;
    m_pNetAM = NULL;
    m_pNetCache = NULL;
    m_updateTimer = 0;

    setInstance(this);

    init();
}

MainWindow::~MainWindow()
{
    if (m_qpRepo)
    {
        delete m_qpRepo;
    }

    //if (m_pNetCache) { delete m_pNetCache;}
    if (m_pNetAM) { delete m_pNetAM; }
    cleanupDocks();
}

void MainWindow::cleanupDocks()
{
    QMapIterator<QString, QDockWidget*> i(m_docks);
    while (i.hasNext()) {
        i.next();
        QDockWidget *pDock = i.value();
        delete pDock;
    }

    m_docks.clear();

}

void MainWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);

    writeSettings();
}

void MainWindow::about()
{
   /*QMessageBox::about(this, tr("About GitBusyLivin"),
            tr("Hope is a good thing, maybe the best of things, and no good thing ever dies."));*/
   AboutDialog about(this);
   about.exec();
}

void MainWindow::clone()
{
    CloneDialog cloneDlg(this);
    if (cloneDlg.exec() == QDialog::Accepted)
    {
        QString src = cloneDlg.getSource();
        QString dst = cloneDlg.getDestination();

        QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

        if (m_qpRepo->clone_repo(src, dst))
        {
            setupRepoUI(dst);
        }
        else
        {
            QMessageBox::warning(this, tr("Clone Error"), m_qpRepo->get_error_msg());
        }

        QApplication::restoreOverrideCursor();
    }
}

void MainWindow::new_local_repo()
{
    QString dirName = QFileDialog::getExistingDirectory(this, tr("Open Directory"),QString(), QFileDialog::ShowDirsOnly);
    if (!dirName.isEmpty())
    {
        if (!m_qpRepo->init_repo(dirName))
        {
            QMessageBox::warning(this, tr("Creation Error"), m_qpRepo->get_error_msg());
        }
        else
        {
            if (m_qpRepo->open_repo(dirName))
            {
                setupRepoUI(dirName);
            }
        }
    }
}

void MainWindow::new_network_repo()
{
    QString dirName = QFileDialog::getExistingDirectory(this, tr("Open Directory"),QString(), QFileDialog::ShowDirsOnly);
    if (!dirName.isEmpty())
    {
        if (!m_qpRepo->init_repo(dirName, true))
        {
            QMessageBox::warning(this, tr("Creation Error"), m_qpRepo->get_error_msg());
        }
    }
}

void MainWindow::open()
{
    QString dirName = QFileDialog::getExistingDirectory(this);
    if (!dirName.isEmpty())
    {
        if (m_qpRepo->open_repo(dirName))
        {
            MainWindow::prependToRecentRepos(dirName);
            m_sRepoPath = dirName;
            if (m_updateTimer) killTimer(m_updateTimer);
            m_updateTimer = startTimer(UPDATE_STATUS_INTERVAL);

            setupRepoUI(dirName);
        }
        else
        {
            QMessageBox::warning(this, tr("Open Error"), m_qpRepo->get_error_msg());
        }
    }
}

void MainWindow::openRecentRepo()
{
    if (const QAction *action = qobject_cast<const QAction *>(sender()))
    {
        QString dirName = action->data().toString();
        if (m_qpRepo->open_repo(dirName))
        {
            MainWindow::prependToRecentRepos(dirName);
            m_sRepoPath = dirName;
            if (m_updateTimer) killTimer(m_updateTimer);
            m_updateTimer = startTimer(UPDATE_STATUS_INTERVAL);

            setupRepoUI(dirName);
        }
        else
        {
            QMessageBox::warning(this, tr("Open Error"), m_qpRepo->get_error_msg());
        }

    }
}

void MainWindow::setupRepoUI(QString repoDir)
{
    if (m_docks.isEmpty())
    {
        createDocks();
        m_pBranchCombo = new ToolbarCombo(m_pToolBar);
        //m_pBranchCombo->addItem(tr("Master"));
        m_pToolBar->addSeparator();
        m_pToolBar->addWidget(m_pBranchCombo);
    }

    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

    QFileInfo fi(repoDir);
    QString title(fi.fileName());
    QTextStream(&title) << " - " << GBL_APP_NAME;
    setWindowTitle(title);

    GBL_History_Array *pHistArr = NULL;
    m_qpRepo->get_history(&pHistArr);

    m_actionMap["refresh"]->setDisabled(false);

    QStringList remotes;
    m_qpRepo->get_remotes(remotes);
    if (m_qpRepo->fill_references())
    {
        m_pBranchCombo->clear();
        m_pBranchCombo->addItems(m_qpRepo->getBranchNames());
        m_pBranchCombo->adjustSize();

       QDockWidget *pDock =  m_docks["refs"];
       ReferencesView *pRefView = (ReferencesView*)pDock->widget();
       pRefView->setRefRoot(m_qpRepo->get_references());
    }


    if (pHistArr != NULL && !pHistArr->isEmpty())
    {
        m_pHistModel->setModelData(pHistArr);
    }

    updateStatus();

    QApplication::restoreOverrideCursor();

}

void MainWindow::updateStatus()
{
    GBL_File_Array stagedArr, unstagedArr;
    if (m_qpRepo->get_repo_status(stagedArr, unstagedArr))
    {
        QDockWidget *pDock = m_docks["staged"];
        StagedDockView *pView = (StagedDockView*)pDock->widget();
        pDock = m_docks["unstaged"];
        UnstagedDockView *pUSView = (UnstagedDockView*)pDock->widget();

        pView->reset();
        pUSView->reset();
        pView->setFileArray(&stagedArr);
        pUSView->setFileArray(&unstagedArr);
    }
}

void MainWindow::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_updateTimer)
    {
        updateStatus();
    }
}

void MainWindow::historySelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    Q_UNUSED(deselected);
    QModelIndexList mil = selected.indexes();

    //single row selection
    if (mil.count() > 0)
    {
        QModelIndex mi = mil.at(0);
        int row = mi.row();
        GBL_History_Item *pHistItem = m_pHistModel->getHistoryItemAt(row);
        if (pHistItem)
        {
            QDockWidget *pDock = m_docks["history_details"];
            QSplitter *pSplit = (QSplitter*)pDock->widget();
            FileView *pView = (FileView*)pSplit->widget(1);
            CommitDetailScrollArea *pDetail = (CommitDetailScrollArea*)pSplit->widget(0);
            pDetail->setDetails(pHistItem, m_pHistModel->getAvatar(pHistItem->hist_author_email));
            pView->reset();
            GBL_FileModel *pMod = (GBL_FileModel*)pView->model();
            pMod->cleanFileArray();
            pMod->setHistoryItem(pHistItem);
            pDock = m_docks["file_diff"];
            if (m_sSelectedCode == "history")
            {
                DiffView *pDV = (DiffView*)pDock->widget();
                pDV->reset();
            }
            //m_qpRepo->get_tree_from_commit_oid(pHistItem->hist_oid, pMod);
            GBL_File_Array histFileArr;
            if (m_qpRepo->get_commit_to_parent_diff_files(pHistItem->hist_oid, &histFileArr))
            {
                pMod->setFileArray(&histFileArr);
            }
        }
    }
}

void MainWindow::historyFileSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    Q_UNUSED(deselected);
    QModelIndexList mil = selected.indexes();


    //single row selection
    if (mil.count() > 0)
    {
        m_sSelectedCode = "history";
        FileView *pFView = m_fileviews["unstaged"];
        pFView->selectionModel()->clearSelection();
        pFView = m_fileviews["staged"];
        pFView->selectionModel()->clearSelection();

        QModelIndex mi = mil.at(0);
        int row = mi.row();
        QDockWidget *pDock = m_docks["history_details"];
        QSplitter *pSplit = (QSplitter*)pDock->widget();
        FileView *pView = (FileView*)pSplit->widget(1);
        GBL_FileModel *pFileMod = (GBL_FileModel*)pView->model();
        GBL_File_Item *pFileItem = pFileMod->getFileItemAt(row);
        if (pFileItem)
        {
            QString path;
            QString sub;
            if (pFileItem->sub_dir != '.')
            {
                sub = pFileItem->sub_dir;
                sub += '/';
            }
            QTextStream(&path) << sub << pFileItem->file_name;
            QByteArray baPath = path.toUtf8();
            GBL_History_Item *pHistItem = pFileMod->getHistoryItem();
            QDockWidget *pDock = m_docks["file_diff"];
            DiffView *pDV = (DiffView*)pDock->widget();
            pDV->reset();

            if (m_qpRepo->get_commit_to_parent_diff_lines(pHistItem->hist_oid, this, baPath.data()))
            {
                pDV->setDiffFromLines(pFileItem);
            }

        }
    }
}

void MainWindow::workingFileSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    Q_UNUSED(deselected);
    Q_UNUSED(selected);


    QDockWidget *pDock = m_docks["file_diff"];
    DiffView *pDV = (DiffView*)pDock->widget();
    pDV->reset();

    FileView *pFView = m_fileviews["unstaged"];
    QModelIndexList mil = pFView->selectionModel()->selectedRows();

   /* QMap<int,int> rowMap;
    for (int i=0; i<mil.size(); i++)
    {
        rowMap[mil.at(i).row()] = 1;
    }

    int count = rowMap.size();*/
    if  (mil.size())
    {
        m_sSelectedCode = "unstaged";
        pFView = m_fileviews["history"];
        pFView->selectionModel()->clearSelection();
        pFView = m_fileviews["staged"];
        pFView->selectionModel()->clearSelection();

        QStringList files;
        QString sPath;
        pDock = m_docks["unstaged"];
        UnstagedDockView *pUSView = (UnstagedDockView*)pDock->widget();
        GBL_File_Array *pFileArr = pUSView->getFileArray();
        GBL_File_Item *pFileItem = NULL;

        QMap<int,int> rowMap;
        for (int i=0; i < mil.size(); i++)
        {
            sPath = "";
            int row = mil.at(i).row();
            if (rowMap.contains(row)) continue;

            rowMap[row] = 1;
            pFileItem = pFileArr->at(row);
            if (pFileItem->sub_dir != ".")
            {
                sPath += pFileItem->sub_dir;
                sPath += "/";
                if (pFileItem->file_name.isEmpty()) sPath += "*";
            }

            sPath += pFileItem->file_name;
            qDebug() << sPath;
            files.append(sPath);
        }

            //qDebug() << path;

        if (m_qpRepo->get_index_to_work_diff(this,&files))
        {
            if (mil.size() > 1) { pFileItem = NULL; }
            pDV->setDiffFromLines(pFileItem);
        }
    }
}

void MainWindow::stagedFileSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    Q_UNUSED(deselected);
    Q_UNUSED(selected);

    QDockWidget *pDock = m_docks["file_diff"];
    DiffView *pDV = (DiffView*)pDock->widget();
    pDV->reset();

    FileView *pFView = m_fileviews["staged"];
    QModelIndexList mil = pFView->selectionModel()->selectedRows();

    if  (mil.size())
    {
        m_sSelectedCode = "staged";
        pFView = m_fileviews["history"];
        pFView->selectionModel()->clearSelection();
        pFView = m_fileviews["unstaged"];
        pFView->selectionModel()->clearSelection();


        QStringList files;
        QString sPath;
        pDock = m_docks["staged"];
        StagedDockView *pSView = (StagedDockView*)pDock->widget();
        GBL_File_Array *pFileArr = pSView->getFileArray();
        GBL_File_Item *pFileItem = NULL;

        QMap<int,int> rowMap;
        for (int i=0; i < mil.size(); i++)
        {
            sPath = "";
            int row = mil.at(i).row();
            if (rowMap.contains(row)) continue;

            rowMap[row] = 1;
            pFileItem = pFileArr->at(row);
            if (pFileItem->sub_dir != ".")
            {
                sPath += pFileItem->sub_dir;
                sPath += "/";
                if (pFileItem->file_name.isEmpty()) sPath += "*";
            }

            sPath += pFileItem->file_name;
            qDebug() << sPath;
            files.append(sPath);
        }


        if (m_qpRepo->get_index_to_head_diff(this, &files))
        {
            if (mil.size() > 1) { pFileItem = NULL; }
            pDV->setDiffFromLines(pFileItem);
        }
    }
}

void MainWindow::stageAll()
{
    QStringList files;
    QDockWidget *pDock = m_docks["unstaged"];
    UnstagedDockView *pUSView = (UnstagedDockView*)pDock->widget();
    GBL_File_Array *pFileArr = pUSView->getFileArray();
    QString sPath;
    for (int i=0; i < pFileArr->size(); i++)
    {
        sPath = "";
        GBL_File_Item *pFileItem = pFileArr->at(i);
        if (pFileItem->sub_dir != ".")
        {
            sPath += pFileItem->sub_dir;
            sPath += "/";
            if (pFileItem->file_name.isEmpty()) sPath += "*";
        }

        sPath += pFileItem->file_name;
        qDebug() << "stageAll_path:" << sPath;
        files.append(sPath);
    }

    if (m_qpRepo->add_to_index(&files))
    {
        updateStatus();
    }
    else
    {
        QMessageBox::warning(this, tr("Open Error"), m_qpRepo->get_error_msg());
    }
}

void MainWindow::stageSelected()
{
    QStringList files;
    QDockWidget *pDock = m_docks["unstaged"];
    UnstagedDockView *pUSView = (UnstagedDockView*)pDock->widget();
    FileView *pFView = pUSView->getFileView();
    QModelIndexList mil = pFView->selectionModel()->selectedRows();
    GBL_File_Array *pFileArr = pUSView->getFileArray();
    QString sPath;
    for (int i=0; i < mil.size(); i++)
    {

        sPath = "";
        GBL_File_Item *pFileItem = pFileArr->at(mil.at(i).row());
        if (pFileItem->sub_dir != ".")
        {
            sPath += pFileItem->sub_dir;
            sPath += "/";
            if (pFileItem->file_name.isEmpty()) sPath += "*";
        }

        sPath += pFileItem->file_name;
        qDebug() << "stageSel_path:" << sPath;
        files.append(sPath);
    }

    if (m_qpRepo->add_to_index(&files))
    {
        updateStatus();
    }
    else
    {
        QMessageBox::warning(this, tr("Open Error"), m_qpRepo->get_error_msg());
    }
}


void MainWindow::unstageAll()
{
    QStringList files;
    QDockWidget *pDock = m_docks["staged"];
    StagedDockView *pSView = (StagedDockView*)pDock->widget();
    GBL_File_Array *pFileArr = pSView->getFileArray();
    QString sPath;
    for (int i=0; i < pFileArr->size(); i++)
    {
        sPath = "";
        GBL_File_Item *pFileItem = pFileArr->at(i);
        if (pFileItem->sub_dir != ".")
        {
            sPath += pFileItem->sub_dir;
            sPath += "/";
            if (pFileItem->file_name.isEmpty()) sPath += "*";
        }

        sPath += pFileItem->file_name;
        //qDebug() << "stageAll_path:" << sPath;
        files.append(sPath);
    }

    if (m_qpRepo->index_unstage(&files))
    {
        updateStatus();
    }
    else
    {
        QMessageBox::warning(this, tr("Open Error"), m_qpRepo->get_error_msg());
    }
}

void MainWindow::unstageSelected()
{
    QStringList files;
    QDockWidget *pDock = m_docks["staged"];
    StagedDockView *pSView = (StagedDockView*)pDock->widget();
    GBL_File_Array *pFileArr = pSView->getFileArray();
    FileView *pFView = pSView->getFileView();
    QModelIndexList mil = pFView->selectionModel()->selectedRows();

    QString sPath;
    for (int i=0; i < mil.size(); i++)
    {
        sPath = "";
        GBL_File_Item *pFileItem = pFileArr->at(mil.at(i).row());
        if (pFileItem->sub_dir != ".")
        {
            sPath += pFileItem->sub_dir;
            sPath += "/";
            if (pFileItem->file_name.isEmpty()) sPath += "*";
        }

        sPath += pFileItem->file_name;
        //qDebug() << "stageAll_path:" << sPath;
        files.append(sPath);
    }

    if (m_qpRepo->index_unstage(&files))
    {
        updateStatus();
    }
    else
    {
        QMessageBox::warning(this, tr("Open Error"), m_qpRepo->get_error_msg());
    }
}

void MainWindow::commit()
{
    QDockWidget *pDock = m_docks["staged"];
    StagedDockView *pSView = (StagedDockView*)pDock->widget();
    QString msg = pSView->getCommitMessage();

    if (m_qpRepo->commit_index(msg))
    {
        setupRepoUI(m_sRepoPath);
    }
    else
    {
        QMessageBox::warning(this, tr("Creation Error"), m_qpRepo->get_error_msg());
    }
}

void MainWindow::addToDiffView(GBL_Line_Item *pLineItem)
{
    QDockWidget *pDock = m_docks["file_diff"];
    DiffView *pDV = (DiffView*)pDock->widget();

    pDV->addLine(pLineItem);
}

void MainWindow::preferences()
{
    QString currentTheme = m_sTheme;

    GBL_Config_Map *pConfigMap;
    if (!m_qpRepo->get_global_config_info(&pConfigMap))
    {
       QMessageBox::warning(this, tr("Config Error"), m_qpRepo->get_error_msg());
    }

    PrefsDialog prefsDlg(this);
    prefsDlg.setConfigMap(pConfigMap);
    if (prefsDlg.exec() == QDialog::Accepted)
    {
        QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());
        settings.setValue("UI/Theme", m_sTheme);

        int nTBType = prefsDlg.getUIToolbarButtonType();
        settings.setValue("UI/Toolbar_text", nTBType);
        Qt::ToolButtonStyle nTBStyle;
        switch (nTBType)
        {
            case 1:
                nTBStyle = Qt::ToolButtonTextBesideIcon;
                break;
            default:
                nTBStyle = Qt::ToolButtonIconOnly;
                break;
        }

        m_pToolBar->setToolButtonStyle(nTBStyle);
        m_pPullBtn->setToolButtonStyle(nTBStyle);
        m_pPushBtn->setToolButtonStyle(nTBStyle);

        //check if global config matches
        GBL_Config_Map cfgMap;
        prefsDlg.getConfigMap(&cfgMap);

        if (cfgMap != *pConfigMap)
        {
            m_qpRepo->set_global_config_info(&cfgMap);
        }
    }
    else
    {
        setTheme(currentTheme);
    }
}

void MainWindow::toggleStatusBar()
{
    if (statusBar()->isVisible())
    {
        statusBar()->hide();
    }
    else
    {
       statusBar()->show();
    }
}

void MainWindow::toggleToolBar()
{
    if (m_pToolBar->isVisible())
    {
        m_pToolBar->hide();
    }
    else
    {
        m_pToolBar->show();
    }
}

void MainWindow::refresh()
{
    updateStatus();
}

void MainWindow::init()
{
    m_qpRepo = new GBL_Repository();
    connect(m_qpRepo,&GBL_Repository::cleaningRepo,this,&MainWindow::cleaningRepo);
    m_pToolBar = addToolBar(tr(GBL_APP_NAME));
    m_pToolBar->setObjectName("MainWindow/Toolbar");
    m_pToolBar->setIconSize(QSize(16,16));
    //m_pToolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    createActions();
    createHistoryTable();
    //createDocks();
    setWindowTitle(tr(GBL_APP_NAME));
    statusBar()->showMessage(tr("Ready"));
    readSettings();

    setWindowIcon(QIcon(QPixmap(QLatin1String(":/images/git_busy_livin_logo_16.png"))));
}

void MainWindow::cleaningRepo()
{
    qDebug() << "cleaning Repo";

    if (!m_docks.isEmpty())
    {
        m_pHistModel->setModelData(NULL);
        m_pHistView->reset();
        m_pHistView->scrollToTop();
        QDockWidget *pDock = m_docks["history_details"];
        QSplitter *pSplit = (QSplitter*)pDock->widget();
        CommitDetailScrollArea *pDetailSA = (CommitDetailScrollArea*)pSplit->widget(0);
        pDetailSA->reset();
        FileView *pView = (FileView*)pSplit->widget(1);
        pView->reset();
        GBL_FileModel *pMod = (GBL_FileModel*)pView->model();
        pMod->cleanFileArray();

        pDock = m_docks["file_diff"];
        DiffView *pDV = (DiffView*)pDock->widget();
        pDV->reset();

        pDock = m_docks["refs"];
        ReferencesView* pRefView = (ReferencesView*)pDock->widget();
        pRefView->setRefRoot(NULL);
        pRefView->reset();
    }
}

void MainWindow::createActions()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QMenu *newMenu = fileMenu->addMenu(tr("&New"));
    QAction *act = newMenu->addAction(tr("&Local Repository..."), this, &MainWindow::new_local_repo);
    act->setStatusTip(tr("Create New Local Repository"));
    act = newMenu->addAction(tr("&Network Repository..."), this, &MainWindow::new_network_repo);
    act->setStatusTip(tr("Create New Bare Remote Repository"));
    act = fileMenu->addAction(tr("&Clone..."), this, &MainWindow::clone);
    act->setStatusTip(tr("Clone a Repository"));
    m_actionMap["clone"] = act;
    act = fileMenu->addAction(tr("&Open..."), this, &MainWindow::open);
    m_actionMap["open"] = act;
    act->setStatusTip(tr("Open a Repository"));
    fileMenu->addSeparator();

    QMenu *recentMenu = fileMenu->addMenu(tr("Recent"));
    connect(recentMenu, &QMenu::aboutToShow, this, &MainWindow::updateRecentRepoActions);
    m_pRecentRepoSubMenuAct = recentMenu->menuAction();

    for (int i = 0; i < MaxRecentRepos; ++i) {
        m_pRecentRepoActs[i] = recentMenu->addAction(QString(), this, &MainWindow::openRecentRepo);
        m_pRecentRepoActs[i]->setVisible(false);
    }

    m_pRecentRepoSeparator = fileMenu->addSeparator();

    setRecentReposVisible(MainWindow::hasRecentRepos());


 #ifdef Q_OS_WIN
    QString sQuit = tr("&Exit");
 #else
    QString sQuit = tr("&Quit");
 #endif
    QAction *quitAct = fileMenu->addAction(sQuit, this, &QWidget::close);
    quitAct->setShortcuts(QKeySequence::Quit);
    quitAct->setStatusTip(tr("Quit the application"));

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(tr("&Preferences..."), this, &MainWindow::preferences);

    m_pRepoMenu = menuBar()->addMenu(tr("&Repository"));
    QAction *refreshAct = m_pRepoMenu->addAction(tr("&Refresh"),this, &MainWindow::refresh);
    refreshAct->setShortcut(QKeySequence(QKeySequence::Refresh));
    refreshAct->setDisabled(true);
    m_actionMap["refresh"] = refreshAct;

#ifdef QT_DEBUG
    QMenu *dbgMenu = menuBar()->addMenu(tr("&Debug"));
    dbgMenu->addAction(tr("&ssl version..."), this, &MainWindow::sslVersion);
#endif

    m_pViewMenu = menuBar()->addMenu(tr("&View"));
    QAction *tbAct = m_pViewMenu->addAction(tr("&Toolbar"));
    QAction *sbAct = m_pViewMenu->addAction(tr("&Statusbar"));
    tbAct->setCheckable(true);
    tbAct->setChecked(true);
    sbAct->setCheckable(true);
    sbAct->setChecked(true);
    connect(tbAct, &QAction::toggled, this, &MainWindow::toggleToolBar);
    connect(sbAct, &QAction::toggled, this, &MainWindow::toggleStatusBar);
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About GitBusyLivin"), this, &MainWindow::about);
}

void MainWindow::sslVersion()
{
    QMessageBox::information(this,tr("ssl version"), QSslSocket::sslLibraryBuildVersionString());
}

void MainWindow::createHistoryTable()
{
    m_pHistModel = new GBL_HistoryModel(NULL, this);
    m_pHistView = new HistoryView(this);
    m_pHistView->setModel(m_pHistModel);
    m_pHistView->setItemDelegateForColumn(0,new HistoryDelegate(m_pHistView));
    m_pHistView->verticalHeader()->hide();
    //m_pHistView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_pHistView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pHistView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pHistView->setShowGrid(false);
    m_pHistView->setAlternatingRowColors(true);
    connect(m_pHistView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::historySelectionChanged);
    setCentralWidget(m_pHistView);
    m_pHistView->setObjectName("MainWindow/HistoryTable");
}

void MainWindow::createDocks()
{
    //setup history details dock
    QDockWidget *pDock = new QDockWidget(tr("History - Details"), this);
    QSplitter *pDetailSplit = new QSplitter(Qt::Vertical, pDock);
    pDetailSplit->setFrameStyle(QFrame::StyledPanel);
    CommitDetailScrollArea *pScroll = new CommitDetailScrollArea(pDetailSplit);
    FileView *pView = new FileView(pDetailSplit);
    m_fileviews["history"] = pView;
    pDetailSplit->addWidget(pScroll);
    pDetailSplit->addWidget(pView);
    pDock->setWidget(pDetailSplit);
    pView->setModel(new GBL_FileModel(pView));
    connect(pView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::historyFileSelectionChanged);
    m_docks["history_details"] = pDock;
    pDock->setObjectName("MainWindow/HistoryDetails");
    addDockWidget(Qt::BottomDockWidgetArea, pDock);
    m_pViewMenu->addAction(pDock->toggleViewAction());

    //setup file differences dock
    pDock = new QDockWidget(tr("Differences"), this);
    DiffView *pDV = new DiffView(pDock);
    pDock->setWidget(pDV);
    m_docks["file_diff"] = pDock;
    pDock->setObjectName("MainWindow/Differences");
    addDockWidget(Qt::BottomDockWidgetArea, pDock);
    m_pViewMenu->addAction(pDock->toggleViewAction());

    //setup staged dock
    pDock = new QDockWidget(tr("Staged"));
    m_docks["staged"] = pDock;
    pDock->setObjectName("MainWindow/Staged");
    addDockWidget(Qt::RightDockWidgetArea, pDock);
    m_pViewMenu->addAction(pDock->toggleViewAction());
    StagedDockView *pSDView = new StagedDockView(pDock);
    pDock->setWidget(pSDView);
    m_pViewMenu->addAction(pDock->toggleViewAction());
    pView = pSDView->getFileView();
    m_fileviews["staged"] = pView;
    connect(pView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::stagedFileSelectionChanged);

    //setup unstaged dock
    pDock = new QDockWidget(tr("Unstaged"));
    m_docks["unstaged"] = pDock;
    pDock->setObjectName("MainWindow/Unstaged");
    addDockWidget(Qt::RightDockWidgetArea, pDock);
    m_pViewMenu->addAction(pDock->toggleViewAction());
    UnstagedDockView *pUSView = new UnstagedDockView(pDock);
    pDock->setWidget(pUSView);
    m_pViewMenu->addAction(pDock->toggleViewAction());
    pView = pUSView->getFileView();
    m_fileviews["unstaged"] = pView;
    connect(pView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::workingFileSelectionChanged);

    //setup refs dock
    pDock = new QDockWidget(tr("References"));
    m_docks["refs"] = pDock;
    pDock->setObjectName("MainWindow/Refs");
    addDockWidget(Qt::LeftDockWidgetArea, pDock);
    ReferencesView *pRefView = new ReferencesView(pDock);
    pDock->setWidget(pRefView);
    pRefView->setModel(new GBL_RefsModel(pRefView));
    m_pViewMenu->addAction(pDock->toggleViewAction());
    QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());
    const QByteArray state = settings.value("MainWindow/WindowState", QByteArray()).toByteArray();
    if (!state.isEmpty())
    {
        restoreState(state);
    }

}

void MainWindow::readSettings()
{
    QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());

    const QByteArray state = settings.value("MainWindow/WindowState", QByteArray()).toByteArray();
    if (!state.isEmpty())
    {
        restoreState(state);
    }

    const QByteArray geometry = settings.value("MainWindow/Geometry", QByteArray()).toByteArray();
    if (geometry.isEmpty()) {
        const QRect availableGeometry = QApplication::desktop()->availableGeometry(this);
        resize(availableGeometry.width() / 3, availableGeometry.height() / 2);
        move((availableGeometry.width() - width()) / 2,
             (availableGeometry.height() - height()) / 2);
    } else {
        restoreGeometry(geometry);
    }

    m_pNetAM = new QNetworkAccessManager();

    //create cache dir
    QString sCachePath = GBL_Storage::getCachePath();
    QDir cachePath(sCachePath);
    if (!cachePath.exists())
    {
        cachePath.mkpath(sCachePath);
    }

    m_pNetCache = new QNetworkDiskCache(this);
    m_pNetCache->setCacheDirectory(sCachePath);
    m_pNetAM->setCache(m_pNetCache);

    QString sTheme = settings.value("UI/Theme", "none").toString();

    setTheme(sTheme);

    int nTBType = settings.value("UI/Toolbar_text",0).toInt();
    Qt::ToolButtonStyle nTBStyle;
    switch (nTBType)
    {
        case 1:
            nTBStyle = Qt::ToolButtonTextBesideIcon;
            break;
        default:
            nTBStyle = Qt::ToolButtonIconOnly;
            break;
    }

    m_pToolBar->setToolButtonStyle(nTBStyle);

   /**/
    UrlPixmap svgpix(NULL);


    QStyleOptionToolBar option;
    option.initFrom(m_pToolBar);
    QPalette pal = option.palette;
    QColor txtClr = pal.color(QPalette::Text);
    QString sBorderClr = txtClr.name(QColor::HexRgb);

    m_pToolBar->setIconSize(QSize(16,16));

    svgpix.loadSVGResource(":/images/open_toolbar_icon.svg", sBorderClr, QSize(16,16));
    QAction *pOpenAct = m_actionMap["open"];
    pOpenAct->setIcon(QIcon(*svgpix.getSmallPixmap(16)));
    m_pToolBar->addAction(pOpenAct);


    svgpix.loadSVGResource(":/images/clone_toolbar_icon.svg", sBorderClr, QSize(16,16));
    QAction *pCloneAct = m_actionMap["clone"];
    pCloneAct->setIcon(QIcon(*svgpix.getSmallPixmap(16)));
    m_pToolBar->addAction(pCloneAct);

    svgpix.loadSVGResource(":/images/push_toolbar_icon.svg", sBorderClr, QSize(16,16));

    QAction *pushAct = new QAction(QIcon(*svgpix.getSmallPixmap(16)), tr("&Push"), this);
    m_pRepoMenu->addAction(pushAct);
    //m_pToolBar->addAction(pushAct);
    pushAct->setDisabled(true);
    m_actionMap["push"] = pushAct;
    m_pPushBtn = new BadgeToolButton(m_pToolBar);
    //m_pPushBtn->setBadge(QString("39"));
    m_pPushBtn->setArrowType(1);
    m_pPushBtn->setDefaultAction(pushAct);
    m_pPushBtn->setToolButtonStyle(nTBStyle);
    //m_pPushBtn->setIcon(*svgpix.getSmallPixmap(16));
    m_pToolBar->addWidget(m_pPushBtn);


    svgpix.loadSVGResource(":/images/pull_toolbar_icon.svg", sBorderClr, QSize(16,16));

    QAction *pullAct = new QAction(QIcon(*svgpix.getSmallPixmap(16)), tr("&Pull"), this);
    m_pRepoMenu->addAction(pullAct);
    //m_pToolBar->addAction(pullAct);
    pullAct->setDisabled(true);
    m_actionMap["pull"] = pullAct;
    m_pPullBtn = new BadgeToolButton(m_pToolBar);
    m_pPullBtn->setDefaultAction(pullAct);
    //m_pPullBtn->setBadge(QString("59"));
    m_pPullBtn->setArrowType(2);
    m_pPullBtn->setToolButtonStyle(nTBStyle);
    m_pToolBar->addWidget(m_pPullBtn);

}

void MainWindow::writeSettings()
{
    QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());
    settings.setValue("MainWindow/Geometry", saveGeometry());
    settings.setValue("MainWindow/WindowState", saveState());

}

static inline QString RecentReposKey() { return QStringLiteral("RecentRepoList"); }
static inline QString fileKey() { return QStringLiteral("file"); }

static QStringList readRecentRepos(QSettings &settings)
{
    QStringList result;
    const int count = settings.beginReadArray(RecentReposKey());
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        result.append(settings.value(fileKey()).toString());
    }
    settings.endArray();
    return result;
}

static void writeRecentRepos(const QStringList &files, QSettings &settings)
{
    const int count = files.size();
    settings.beginWriteArray(RecentReposKey());
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        settings.setValue(fileKey(), files.at(i));
    }
    settings.endArray();
}

bool MainWindow::hasRecentRepos()
{
    QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());
    const int count = settings.beginReadArray(RecentReposKey());
    settings.endArray();
    return count > 0;
}

void MainWindow::prependToRecentRepos(const QString &dirName)
{
    QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());

    const QStringList oldRecentRepos = readRecentRepos(settings);
    QStringList RecentRepos = oldRecentRepos;
    RecentRepos.removeAll(dirName);
    RecentRepos.prepend(dirName);
    if (oldRecentRepos != RecentRepos)
        writeRecentRepos(RecentRepos, settings);

    setRecentReposVisible(!RecentRepos.isEmpty());
}

void MainWindow::setRecentReposVisible(bool visible)
{
    m_pRecentRepoSubMenuAct->setVisible(visible);
    m_pRecentRepoSeparator->setVisible(visible);
}

void MainWindow::updateRecentRepoActions()
{
    QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());

    const QStringList RecentRepos = readRecentRepos(settings);
    const int count = qMin(int(MaxRecentRepos), RecentRepos.size());
    int i = 0;
    for ( ; i < count; ++i) {
        const QString fileName = QFileInfo(RecentRepos.at(i)).fileName();
        m_pRecentRepoActs[i]->setText(tr("&%1 %2").arg(i + 1).arg(fileName));
        m_pRecentRepoActs[i]->setData(RecentRepos.at(i));
        m_pRecentRepoActs[i]->setVisible(true);
    }
    for ( ; i < MaxRecentRepos; ++i)
        m_pRecentRepoActs[i]->setVisible(false);
}

void MainWindow::setTheme(const QString &theme)
{
    QString styleSheet;
    QString sPath;

    m_sTheme = theme;

    if (theme != "none")
    {
        if (theme == "shawshank")
        {
            sPath = ":/styles/shawshank.qss";
        }
        else if (theme == "zihuatanejo")
        {
            sPath = ":/styles/zihuatanejo.qss";
        }
        else
        {
            sPath = GBL_Storage::getThemesPath();
            sPath += QDir::separator();
            sPath += theme;
            sPath += ".qss";
        }
    }

    if (!sPath.isEmpty())
    {
        QFile file(sPath);
        file.open(QFile::ReadOnly);
        styleSheet = QString::fromUtf8(file.readAll());
    }

    QApplication *app = (QApplication*)QCoreApplication::instance();
    app->setStyleSheet(styleSheet);

}
