STATUS=ERROR
sby -f test1.inter0007_out.sby > /dev/null && STATUS=$(awk '{print $1}' test1.inter0007_out/status)
echo $STATUS > status
case $STATUS in
    PASS)
        echo "Proved equivalence of partition 'test1.inter0007_out' using strategy 'pdr'"
    ;;
    FAIL)
        echo "Could not prove equivalence of partition 'test1.inter0007_out' using strategy 'pdr': partitions not equivalent"
    ;;
    UNKNOWN)
        echo "Could not prove equivalence of partition 'test1.inter0007_out' using strategy 'pdr': equivalence unknown"
    ;;
    TIMEOUT)
        echo "Could not prove equivalence of partition 'test1.inter0007_out' using strategy 'pdr': timeout"
    ;;
    *)
        cat test1.inter0007_out/ERROR 2> /dev/null
        echo "Execution of strategy 'pdr' on partition 'test1.inter0007_out' encountered an error."
        echo "More details can be found in '/tmp/pmux_nopost_sc3wldoo/proofs_zero_init/test1_baseline_boundary_final/run/strategies/test1.inter0007_out/pdr/test1.inter0007_out/logfile.txt'."
        exit 1
    ;;
esac
exit 0

