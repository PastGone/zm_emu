```bash
//测试单个关键实现
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 00000102
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 00000506
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 00000001 
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 000007ca
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 00000462
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 00000442
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 0000042f
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 0000048a
//测试多个关键实现
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh 00000102 00000506 00000001 000007ca 00000462 00000442 0000042f

//测试所有实现
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=1 ./test_all.sh
//
ZM_TEST_DISPLAY=1 ZM_TEST_SEC=0 ./test_all.sh


//扫描工具
cd ./go_tools/applet_scan
go run . /home/apollo/文档/古时游戏/zm_emu/applet -o ../../applet_headers.csv
```


```pwsh

.\pack_win.ps1                      # 构建 + 打包
.\pack_win.ps1 -NoBuild             # 跳过构建，复用已有 exe
.\pack_win.ps1 -NoZip               # 只生成目录，不压缩（自己先试跑）
.\pack_win.ps1 -Name my_pack -OutDir out   # 自定义包名/输出目录





# 测试单个关键实现
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=0; .\test_all.ps1 00000102
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=0; .\test_all.ps1 00000506
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=0; .\test_all.ps1 00000001
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=0; .\test_all.ps1 000007ca
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=0; .\test_all.ps1 00000462
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=0; .\test_all.ps1 00000442

# 测试多个关键实现
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=0; .\test_all.ps1 00000102 00000506 00000001 000007ca 00000462

# 测试所有实现
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=1; .\test_all.ps1
#
$env:ZM_TEST_DISPLAY=1; $env:ZM_TEST_SEC=0; .\test_all.ps1


# 扫描工具
cd .\go_tools\applet_scan
go run . E:\codePlace\emu\zm_emu\applet -o ..\..\applet_headers.csv

# 提示：若报“禁止运行脚本”，先执行一次 Set-ExecutionPolicy -Scope Process Bypass
```
