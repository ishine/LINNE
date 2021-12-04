" 指定ディレクトリ以下のwavを指定秒ごとに分割する "
import glob
import os
from pathlib import Path
import numpy as np
import scipy.io.wavfile as wv

TEST_FILE_DIR_LIST = [
  "./data/**/*.wav",
]

OUTPUT_DIR = './output'

SPLIT_SECONDS = 10

if __name__ == "__main__":
    filelist = []
    # wavファイルリストを取得
    for d in TEST_FILE_DIR_LIST:
        filelist += glob.glob(d, recursive=True)
    filelist.sort()
    # 出力先ディレクトリ作成
    for file in filelist:
        Path(os.path.dirname(os.path.join(OUTPUT_DIR, file))).mkdir(parents=True, exist_ok=True)
    # wav切り出し
    for file in filelist:
        rate, data = wv.read(file)
        nsmpl = data.shape[0]
        splitsmpl = SPLIT_SECONDS * rate
        # 切り出す時間で切り上げる（末尾は無音）
        extdata = np.zeros((nsmpl - (nsmpl % splitsmpl) + splitsmpl, data.shape[1]),
                dtype=data.dtype)
        extdata[:nsmpl] = data
        basefile = os.path.splitext(file)[0]
        # 指定時間で切り出しつつ出力
        for div in range(nsmpl // splitsmpl + 1):
            filename = basefile + '_%03d.wav' % div
            outpath = os.path.join(OUTPUT_DIR, filename)
            wv.write(outpath, rate, extdata[div * splitsmpl:(div + 1) * splitsmpl])