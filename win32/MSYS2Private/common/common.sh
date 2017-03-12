#!/bin/bash
#共通関数、設定
#このスクリプトはsourceコマンドで実行すること。

function exitOnError(){
if [ $? -ne 0 ]; then
    echo "ERROR."
    exit
else
    echo "SUCCESS."
fi
}

function patchOnce(){
#適用済みでない場合のみパッチを充てる
patch -p$1 -N --dry-run --silent < $2 2>/dev/null
if [ $? -eq 0 ];
then
    #apply the patch
    patch -p$1 -N < $2
fi
}

function exitOnError(){
if [ $? -ne 0 ]; then
    echo "ERROR."
    exit
else
    echo "SUCCESS."
fi
}

function makeParallel(){
#並列ビルドの場合依存関係でビルドに失敗することがあるので3回までmakeする。
for (( i=0; i<3; i++))
do
    start //B //WAIT //LOW mingw32-make -j$NUMBER_OF_PROCESSORS "$@"
    if [ $? -eq 0 ]; then
        return 0
    fi
done
return 1
}

function waitEnter(){
echo "Hit Enter"
read Wait
}

function toolchain(){
#基本ツールチェーン
#ディレクトリが存在しない場合があるので作っておく
mkdir $MINGW_PREFIX 2> /dev/null

#ツール類
pacman -S --needed --noconfirm \
base \
base-devel \
VCS \
unzip \
wget \
tar \
zip \
perl \
python \
ruby \
$MINGW_PACKAGE_PREFIX-toolchain

exitOnError


#このスクリプトの置き場所
PATCH_DIR=$(dirname $(readlink -f ${BASH_SOURCE:-$0}))
#DirectShowのヘッダー問題対策
pushd $MINGW_PREFIX/$MINGW_CHOST
#https://github.com/Alexpux/MINGW-packages/issues/1689
patchOnce 2 $PATCH_DIR/0001-Revert-Avoid-declaring-something-extern-AND-initiali.patch
#https://sourceforge.net/p/mingw-w64/mailman/message/35527066/
patchOnce 2 $PATCH_DIR/wrl.patch
unset PATCH_DIR
popd
}

function commonSetup(){
#共通の環境変数、パスの設定
#環境チェック
if [ -z "$MINGW_PREFIX" ]; then
  echo "Please run this script in MinGW 32bit or 64bit shell. (not in MSYS2 shell)"
  exit 1
fi

#基本ツールチェーンのセットアップ
toolchain

#外部依存ライブラリのソース展開先
mkdir ~/extlib 2> /dev/null
export EXTLIB=~/extlib

#インストール先(/mingw32/localまたは/mingw64/local)
export PREFIX=$MINGW_PREFIX/local
mkdir -p $PREFIX/bin 2> /dev/null

#最低限必要なDLLをコピー
pushd $MINGW_PREFIX/bin
if [ "$MINGW_CHOST" = "i686-w64-mingw32" ]; then
	#32bit
	NEEDED_DLLS='libgcc_s_dw2-1.dll libstdc++-6.dll libwinpthread-1.dll zlib1.dll'
else
	#64bit
	NEEDED_DLLS='libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll zlib1.dll'
fi
cp -f $NEEDED_DLLS $PREFIX/bin
popd
}
