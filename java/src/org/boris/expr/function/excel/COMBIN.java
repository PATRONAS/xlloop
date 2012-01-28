package org.boris.expr.function.excel;

import org.boris.expr.Expr;
import org.boris.expr.ExprDouble;
import org.boris.expr.ExprError;
import org.boris.expr.ExprException;
import org.boris.expr.ExprNumber;
import org.boris.expr.function.AbstractFunction;
import org.boris.expr.util.Statistics;

public class COMBIN extends AbstractFunction
{
    public Expr evaluate(Expr[] args) throws ExprException {
        assertArgCount(args, 2);
        Expr eNum = evalArg(args[0]);
        if (!isNumber(eNum))
            return ExprError.VALUE;
        int num = ((ExprNumber) eNum).intValue();
        Expr eCho = evalArg(args[1]);
        if (!isNumber(eCho))
            return ExprError.VALUE;
        int cho = ((ExprNumber) eCho).intValue();
        if (num < 0 || cho < 0 || num < cho)
            return ExprError.NUM;

        return new ExprDouble(Statistics.combin(num, cho));
    }
}
